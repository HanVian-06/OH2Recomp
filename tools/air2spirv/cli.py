#!/usr/bin/env python3
"""
CLI Driver & SPVC v1 Cache Packer for tools/air2spirv:
1. Extracts all 4,544 AIR 2.0 shaders (317 Global + 4,227 OH2) and 482 pipelines from oh2-ios.pak.
2. Translates all 4,482 unique MTLB modules (covering all 4,544 archive entries) to Vulkan 1.1 SPIR-V 1.3.
3. Validates every SPIR-V module against Vulkan 1.1 (SPIRV-Tools v2026.2 / spirv-val --target-env vulkan1.1).
4. Packs build/shaders/spirv_cache.bin (SPVC v1 format matching src/metal_vk/spirv_cache.h).
5. Writes build/shaders/m4_report.json with full corpus, intrinsic, reflection, and validation metrics.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import time
import zlib
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from typing import Any

from .air_to_spirv import translate_air_shader
from .validator import SPIRV_VAL_EXE, SpirvValidator, validate_with_cli

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_PAK = os.path.join(
    REPO_ROOT, "Payload", "OH2.app", "cookeddata", "oh2", "content", "paks", "oh2-ios.pak"
)
DEFAULT_CACHE_OUT = os.path.join(REPO_ROOT, "build", "shaders", "spirv_cache.bin")
DEFAULT_REPORT_OUT = os.path.join(REPO_ROOT, "build", "shaders", "m4_report.json")

SPIRV_CACHE_MAGIC = 0x43565053  # 'SPVC'
SPIRV_CACHE_VERSION = 1

_WORKER_VALIDATOR: SpirvValidator | None = None


def _read_fstring(buf: bytes, pos: int) -> tuple[str, int]:
    (slen,) = struct.unpack_from("<i", buf, pos)
    pos += 4
    if slen > 0:
        s = buf[pos : pos + slen - 1].decode("utf-8", errors="replace")
        return s, pos + slen
    elif slen < 0:
        nbytes = (-slen) * 2
        s = buf[pos : pos + nbytes - 2].decode("utf-16-le", errors="replace")
        return s, pos + nbytes
    return "", pos


def extract_pak_files(pak_path: str, target_names: set[str]) -> dict[str, bytes]:
    file_size = os.path.getsize(pak_path)
    out: dict[str, bytes] = {}
    with open(pak_path, "rb") as f:
        f.seek(file_size - 221)
        footer = f.read(221)
        _guid, b_enc_idx, magic, version, index_off, index_size = struct.unpack_from("<16sBIIQQ", footer, 0)
        if magic != 0x5A6F12E1 or version != 8 or b_enc_idx != 0:
            raise ValueError(f"Unexpected pak header: magic=0x{magic:x} ver={version} enc={b_enc_idx}")
        f.seek(index_off)
        idx = f.read(index_size)

        pos = 0
        _mount_point, pos = _read_fstring(idx, pos)
        (num_entries,) = struct.unpack_from("<I", idx, pos)
        pos += 4
        for _ in range(num_entries):
            fname, pos = _read_fstring(idx, pos)
            offset, _size, uncomp_size, comp_method = struct.unpack_from("<QQQI", idx, pos)
            pos += 48  # 28 + 20-byte sha1
            blocks = []
            if comp_method != 0:
                (bcount,) = struct.unpack_from("<I", idx, pos)
                pos += 4
                for _ in range(bcount):
                    bstart, bend = struct.unpack_from("<QQ", idx, pos)
                    pos += 16
                    blocks.append((bstart, bend))
            pos += 5  # flags + comp_block_size
            if fname in target_names:
                if comp_method == 0:
                    f.seek(offset + 53)
                    out[fname] = f.read(uncomp_size)
                elif comp_method == 1:
                    buf = bytearray()
                    for bstart, bend in blocks:
                        f.seek(offset + bstart)
                        buf.extend(zlib.decompress(f.read(bend - bstart)))
                    out[fname] = bytes(buf[:uncomp_size])
                else:
                    raise ValueError(f"Unsupported compression method {comp_method}")
    return out


def parse_ushaderbytecode(blob: bytes, archive_label: str) -> list[dict[str, Any]]:
    ver, num_shaders = struct.unpack_from("<II", blob, 0)
    if ver != 1:
        raise ValueError(f"Unexpected ushaderbytecode version {ver}")
    pos = 8
    data_base = 8 + num_shaders * 37
    entries = []
    for i in range(num_shaders):
        sha1 = blob[pos : pos + 20]
        off, comp_sz, uncomp_sz, freq = struct.unpack_from("<QIIB", blob, pos + 20)
        pos += 37
        raw_comp = blob[data_base + off : data_base + off + comp_sz]
        raw = zlib.decompress(raw_comp) if comp_sz != uncomp_sz else raw_comp
        mtlb_idx = raw.find(b"MTLB")
        if mtlb_idx == -1:
            raise ValueError(f"Missing MTLB in {archive_label}[{i}]")
        mtlb = raw[mtlb_idx:]
        mtlb_sha256 = hashlib.sha256(mtlb).digest()
        shader_id = struct.unpack_from("<Q", mtlb_sha256, 0)[0]
        entries.append(
            {
                "archive": archive_label,
                "index": i,
                "pak_sha1": sha1,
                "freq": freq,
                "raw": raw,
                "shader_id": shader_id,
                "mtlb_sha256": mtlb_sha256,
            }
        )
    return entries


def _translate_and_validate_worker(item: tuple[int, int, bytes, bytes]) -> dict[str, Any]:
    global _WORKER_VALIDATOR
    if _WORKER_VALIDATOR is None:
        _WORKER_VALIDATOR = SpirvValidator("vulkan1.1")

    shader_id, freq, pak_sha1, raw = item
    try:
        spirv_bytes, refl = translate_air_shader(raw, freq)
        val_ok, val_err = _WORKER_VALIDATOR.validate(spirv_bytes)
        if val_ok:
            refl["flags"] |= 1  # SPIRV_ENTRY_FLAG_VALIDATED
        return {
            "ok": True,
            "val_ok": val_ok,
            "val_err": val_err,
            "shader_id": shader_id,
            "pak_sha1": pak_sha1,
            "spirv_bytes": spirv_bytes,
            "refl": refl,
        }
    except Exception as ex:
        import traceback

        return {
            "ok": False,
            "val_ok": False,
            "val_err": f"{ex}\n{traceback.format_exc()}",
            "shader_id": shader_id,
            "pak_sha1": pak_sha1,
            "spirv_bytes": b"",
            "refl": None,
        }


def pack_spirv_cache(
    results_by_id: dict[int, dict[str, Any]],
    total_archive_shaders: int,
    total_archive_passed: int,
    out_path: str,
) -> dict[str, Any]:
    """
    Pack translated SPIR-V modules and reflection tables into SPVC v1 binary format
    matching src/metal_vk/spirv_cache.h.
    """
    sorted_ids = sorted(results_by_id.keys())
    entry_count = len(sorted_ids)

    string_table = bytearray(b"\x00")
    string_offsets: dict[str, int] = {"": 0}

    def intern_str(s: str) -> int:
        if s in string_offsets:
            return string_offsets[s]
        off = len(string_table)
        string_table.extend(s.encode("utf-8") + b"\x00")
        string_offsets[s] = off
        return off

    bindings_blob = bytearray()
    vertex_attrs_blob = bytearray()
    spirv_blob = bytearray()

    binding_total = 0
    vertex_attr_total = 0
    prelim_entries = []

    for sid in sorted_ids:
        res = results_by_id[sid]
        refl = res["refl"]
        spv = res["spirv_bytes"]

        b_first = binding_total
        b_list = refl["bindings"] if refl else []
        for b in b_list:
            bindings_blob.extend(
                struct.pack(
                    "<8BI",
                    b["set"] & 0xFF,
                    b["binding"] & 0xFF,
                    b["descriptor_type"] & 0xFF,
                    b["metal_slot"] & 0xFF,
                    b["metal_kind"] & 0xFF,
                    b["image_dim"] & 0xFF,
                    b["is_depth"] & 0xFF,
                    b["flags"] & 0xFF,
                    b["buffer_size_bytes"] & 0xFFFFFFFF,
                )
            )
        binding_total += len(b_list)

        va_first = vertex_attr_total
        va_list = refl["vertex_attrs"] if refl else []
        for va in va_list:
            name_off = intern_str(va["name"])
            vertex_attrs_blob.extend(
                struct.pack(
                    "<4BI",
                    va["location"] & 0xFF,
                    va["component_type"] & 0xFF,
                    va["components"] & 0xFF,
                    va["flags"] & 0xFF,
                    name_off,
                )
            )
        vertex_attr_total += len(va_list)

        entry_name_off = intern_str(refl["entry_name"] if refl else "main")
        spv_rel_off = len(spirv_blob)
        spirv_blob.extend(spv)
        pad = (4 - (len(spirv_blob) & 3)) & 3
        if pad:
            spirv_blob.extend(b"\x00" * pad)

        prelim_entries.append((sid, res, refl, spv_rel_off, len(spv), b_first, len(b_list), va_first, len(va_list), entry_name_off))

    # Align section offsets
    header_size = 80
    entries_offset = header_size  # 80 is 8-byte aligned
    entries_size = entry_count * 112
    bindings_offset = entries_offset + entries_size  # 4-byte aligned
    vertex_attrs_offset = bindings_offset + len(bindings_blob)
    strings_offset = vertex_attrs_offset + len(vertex_attrs_blob)

    # Pad string_table to 4-byte alignment so spirv_blob_offset is 4-byte aligned
    str_pad = (4 - (len(string_table) & 3)) & 3
    if str_pad:
        string_table.extend(b"\x00" * str_pad)
    spirv_blob_offset = strings_offset + len(string_table)
    total_file_size = spirv_blob_offset + len(spirv_blob)

    entries_blob = bytearray()
    unique_translated = 0
    unique_passed = 0

    for sid, res, refl, spv_rel_off, spv_sz, b_first, b_cnt, va_first, va_cnt, name_off in prelim_entries:
        if res["ok"]:
            unique_translated += 1
        if res["val_ok"]:
            unique_passed += 1
        lx, ly, lz = refl["local_size"] if refl else (0, 0, 0)
        entries_blob.extend(
            struct.pack(
                "<Q20s32s8I8HI",
                sid,
                res["pak_sha1"],
                refl["mtlb_sha256"] if refl else (b"\x00" * 32),
                refl["stage"] if refl else 0,
                spirv_blob_offset + spv_rel_off,
                spv_sz,
                b_first,
                va_first,
                name_off,
                refl["buffer_mask"] if refl else 0,
                refl["texture_mask"] if refl else 0,
                refl["sampler_mask"] if refl else 0,
                b_cnt,
                va_cnt,
                refl["fragment_output_mask"] if refl else 0,
                refl["flags"] if refl else 0,
                lx,
                ly,
                lz,
                0,
            )
        )

    hdr_flags = 1 if unique_passed == entry_count else 0
    header_blob = struct.pack(
        "<4I6Q2I8s",
        SPIRV_CACHE_MAGIC,
        SPIRV_CACHE_VERSION,
        hdr_flags,
        entry_count,
        entries_offset,
        bindings_offset,
        vertex_attrs_offset,
        strings_offset,
        spirv_blob_offset,
        total_file_size,
        total_archive_shaders if unique_translated == entry_count else unique_translated,
        total_archive_passed,
        b"\x00" * 8,
    )

    assert len(header_blob) == 80
    assert len(entries_blob) == entries_size

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(header_blob)
        f.write(entries_blob)
        f.write(bindings_blob)
        f.write(vertex_attrs_blob)
        f.write(string_table)
        f.write(spirv_blob)

    return {
        "cache_path": out_path,
        "total_file_size": total_file_size,
        "entry_count": entry_count,
        "binding_records": binding_total,
        "vertex_attr_records": vertex_attr_total,
        "string_table_bytes": len(string_table),
        "spirv_blob_bytes": len(spirv_blob),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="AIR 2.0 LLVM Bitcode -> Vulkan 1.1 SPIR-V Offline Pipeline")
    parser.add_argument("--pak", default=DEFAULT_PAK, help="Path to oh2-ios.pak")
    parser.add_argument("--out", default=DEFAULT_CACHE_OUT, help="Output path for spirv_cache.bin")
    parser.add_argument("--report", default=DEFAULT_REPORT_OUT, help="Output path for m4_report.json")
    parser.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 4))
    parser.add_argument("--cli-sample", type=int, default=32, help="Number of modules to also cross-check via spirv-val.exe CLI")
    args = parser.parse_args(argv)

    t0 = time.time()
    target_names = {
        "OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode",
        "OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode",
        "OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderpipelines",
    }
    pak_blobs = extract_pak_files(args.pak, target_names)
    global_entries = parse_ushaderbytecode(
        pak_blobs["OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode"], "Global"
    )
    oh2_entries = parse_ushaderbytecode(
        pak_blobs["OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode"], "OH2"
    )
    pipelines_blob = pak_blobs["OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderpipelines"]
    pipe_ver, pipe_count = struct.unpack_from("<II", pipelines_blob, 0)

    all_entries = global_entries + oh2_entries
    total_shaders = len(all_entries)

    unique_work: dict[int, tuple[int, int, bytes, bytes]] = {}
    for e in all_entries:
        sid = e["shader_id"]
        if sid not in unique_work:
            unique_work[sid] = (sid, e["freq"], e["pak_sha1"], e["raw"])

    print(
        f"[air2spirv] Extracted {total_shaders} shaders (Global={len(global_entries)}, OH2={len(oh2_entries)}, "
        f"unique_mtlb={len(unique_work)}, pipelines={pipe_count}) in {time.time()-t0:.2f}s"
    )

    t1 = time.time()
    results_by_id: dict[int, dict[str, Any]] = {}
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        for res in ex.map(_translate_and_validate_worker, list(unique_work.values()), chunksize=32):
            results_by_id[res["shader_id"]] = res

    # Optional CLI spirv-val.exe cross-verification on sample
    cli_checked = 0
    cli_passed = 0
    if args.cli_sample > 0 and os.path.isfile(SPIRV_VAL_EXE):
        sample_ids = sorted(results_by_id.keys())[:: max(1, len(results_by_id) // args.cli_sample)][: args.cli_sample]
        for sid in sample_ids:
            r = results_by_id[sid]
            if r["spirv_bytes"]:
                cli_checked += 1
                ok_cli, _ = validate_with_cli(r["spirv_bytes"], "vulkan1.1")
                if ok_cli:
                    cli_passed += 1

    # Compute full archive statistics across all 4,544 entries
    total_translated = 0
    total_passed = 0
    stage_totals: Counter[str] = Counter()
    stage_passed: Counter[str] = Counter()
    failures: list[dict[str, Any]] = []

    for e in all_entries:
        sid = e["shader_id"]
        r = results_by_id[sid]
        st_name = {0: "VS", 3: "PS", 5: "CS"}.get(e["freq"], f"F{e['freq']}")
        stage_totals[st_name] += 1
        if r["ok"]:
            total_translated += 1
        if r["val_ok"]:
            total_passed += 1
            stage_passed[st_name] += 1
        else:
            failures.append(
                {
                    "archive": e["archive"],
                    "index": e["index"],
                    "stage": st_name,
                    "shader_id": f"0x{sid:016x}",
                    "entry_name": r["refl"]["entry_name"] if r["refl"] else "unknown",
                    "error": r["val_err"][:500],
                }
            )

    pass_rate = (total_passed / total_shaders * 100.0) if total_shaders > 0 else 0.0
    unique_passed = sum(1 for r in results_by_id.values() if r["val_ok"])
    unique_translated = sum(1 for r in results_by_id.values() if r["ok"])

    cache_info = pack_spirv_cache(results_by_id, total_shaders, total_passed, args.out)

    report = {
        "total_shaders": total_shaders,
        "global_shaders": len(global_entries),
        "oh2_shaders": len(oh2_entries),
        "pipelines_version": pipe_ver,
        "pipelines_count": pipe_count,
        "unique_mtlb": len(unique_work),
        "unique_translated": unique_translated,
        "unique_spirv_val_passed": unique_passed,
        "translated_count": total_translated,
        "spirv_val_passed_count": total_passed,
        "pass_rate_percent": round(pass_rate, 4),
        "stage_totals": dict(stage_totals),
        "stage_passed": dict(stage_passed),
        "cli_spirv_val_sample_checked": cli_checked,
        "cli_spirv_val_sample_passed": cli_passed,
        "cache_binary": cache_info,
        "elapsed_seconds": round(time.time() - t0, 2),
        "failures_count": len(failures),
        "failures_sample": failures[:25],
    }

    os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as rf:
        json.dump(report, rf, indent=2)

    print(
        f"[air2spirv] Translated {total_translated}/{total_shaders} ({unique_translated}/{len(unique_work)} unique) | "
        f"spirv-val (vulkan1.1) passed: {total_passed}/{total_shaders} ({pass_rate:.2f}%) | "
        f"CLI sample: {cli_passed}/{cli_checked} | cache: {cache_info['total_file_size']:,} bytes in {time.time()-t1:.2f}s"
    )
    if failures:
        print(f"[air2spirv] Failures ({len(failures)} total, showing up to 10):")
        for fl in failures[:10]:
            print(f"  - {fl['archive']}[{fl['index']}] ({fl['stage']}) {fl['entry_name']}: {fl['error']}")

    return 0 if pass_rate >= 90.0 else 1


if __name__ == "__main__":
    sys.exit(main())
