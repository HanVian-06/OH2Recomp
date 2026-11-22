#!/usr/bin/env python3
"""
Reproducible M0 inspection of Payload/OH2.app/cookeddata/oh2/content/paks/oh2-ios.pak:
- UE4 Pak v8 footer & index (19,920 entries, bEncryptedIndex=0, compression breakdown)
- Extension & directory size breakdown
- Texture PF_* format scan across all .uasset files
- Sampled Audio RIFF/WAVE ADPCM verification + INI config checks
- Full inspection of all 4,544 shaders in .ushaderbytecode (MTLB + AIR 2.0 BC\\xc0\\xde,
  0 MSL source, 0 tessellation/raytracing/tile/argument-buffers/function-constants)
"""

import argparse
import json
import os
import re
import struct
import sys
import zlib
from collections import Counter, defaultdict

DEFAULT_PAK = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "Payload",
    "OH2.app",
    "cookeddata",
    "oh2",
    "content",
    "paks",
    "oh2-ios.pak",
)

PF_NAMES = [
    b"PF_ASTC_4x4",
    b"PF_ASTC_6x6",
    b"PF_ASTC_8x8",
    b"PF_ASTC_10x10",
    b"PF_ASTC_12x12",
    b"PF_B8G8R8A8",
    b"PF_G8",
    b"PF_FloatRGBA",
    b"PF_DXT1",
    b"PF_DXT5",
    b"PF_BC5",
    b"PF_ETC2_RGB",
    b"PF_ETC2_RGBA",
    b"PF_PVRTC2",
    b"PF_PVRTC4",
]


def read_fstring(buf: bytes, pos: int) -> tuple[str, int]:
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


def parse_pak_v8(pak_path: str):
    file_size = os.path.getsize(pak_path)
    with open(pak_path, "rb") as f:
        f.seek(file_size - 221)
        footer = f.read(221)
        guid, b_enc_idx, magic, version, index_off, index_size = struct.unpack_from("<16sBIIQQ", footer, 0)
        assert magic == 0x5A6F12E1, f"Unexpected pak magic 0x{magic:x}"
        assert version == 8, f"Unexpected pak version {version}"
        assert b_enc_idx == 0, f"Pak index is encrypted ({b_enc_idx})"
        comp_methods = [
            footer[61 + i * 32 : 61 + (i + 1) * 32].split(b"\x00", 1)[0].decode("ascii", errors="replace")
            for i in range(5)
        ]

        f.seek(index_off)
        idx = f.read(index_size)

    pos = 0
    mount_point, pos = read_fstring(idx, pos)
    (num_entries,) = struct.unpack_from("<I", idx, pos)
    pos += 4

    entries = []
    for _ in range(num_entries):
        fname, pos = read_fstring(idx, pos)
        offset, size, uncomp_size, comp_method = struct.unpack_from("<QQQI", idx, pos)
        pos += 28
        pos += 20  # SHA1 hash
        blocks = []
        if comp_method != 0:
            (bcount,) = struct.unpack_from("<I", idx, pos)
            pos += 4
            for _ in range(bcount):
                bstart, bend = struct.unpack_from("<QQ", idx, pos)
                pos += 16
                blocks.append((bstart, bend))
        flags, comp_block_size = struct.unpack_from("<BI", idx, pos)
        pos += 5
        entries.append(
            {
                "name": fname,
                "offset": offset,
                "size": size,
                "uncompressed_size": uncomp_size,
                "comp_method": comp_method,
                "blocks": blocks,
                "flags": flags,
                "comp_block_size": comp_block_size,
            }
        )

    header = {
        "file_size": file_size,
        "magic": f"0x{magic:08x}",
        "version": version,
        "bEncryptedIndex": b_enc_idx,
        "index_offset": index_off,
        "index_size": index_size,
        "mount_point": mount_point,
        "num_entries": num_entries,
        "compression_methods": comp_methods,
    }
    return header, entries


def extract_entry(f, entry: dict) -> bytes:
    if entry["comp_method"] == 0:
        # Skip inline FPakEntry header: 8+8+8+4+20+1+4 = 53 bytes
        f.seek(entry["offset"] + 53)
        return f.read(entry["uncompressed_size"])
    elif entry["comp_method"] == 1:
        out = bytearray()
        for bstart, bend in entry["blocks"]:
            f.seek(entry["offset"] + bstart)
            chunk = f.read(bend - bstart)
            out.extend(zlib.decompress(chunk))
        return bytes(out[: entry["uncompressed_size"]])
    else:
        raise ValueError(f"Unsupported compression method {entry['comp_method']}")


def parse_mtlb_tags(mtlb: bytes) -> dict:
    magic, ver_maj, ver_min, ver_patch, unk, flags_or_len, total_size = struct.unpack_from("<4sHHHHII", mtlb, 0)
    # MTLB header layout: 4s('MTLB'), H(ver_major?), H(ver_minor?), H(ver_patch?), ...
    # Parse header cleanly: 88-byte header on MTLB v1.2.x
    # Actually let's read header fields safely:
    # bytes 4..12: 4x uint16 version/flags
    v = struct.unpack_from("<4H", mtlb, 4)
    # Find function list offset: at offset 24 (uint64 fn_list_off, uint64 fn_list_sz)
    fn_off, fn_sz = struct.unpack_from("<QQ", mtlb, 24)
    bc_off, bc_sz = struct.unpack_from("<QQ", mtlb, 72)

    tags = {}
    if fn_off < len(mtlb):
        # fn_list starts with uint32 count, uint32 tag_group_len
        p = fn_off + 8
        end = min(len(mtlb), fn_off + fn_sz)
        while p + 4 <= end:
            tag = mtlb[p : p + 4]
            p += 4
            if tag == b"ENDT":
                break
            if p + 2 > end:
                break
            tlen = struct.unpack_from("<H", mtlb, p)[0]
            p += 2
            val = mtlb[p : p + tlen]
            p += tlen
            tags[tag.decode("ascii", errors="replace")] = val

    bc_bytes = mtlb[bc_off : bc_off + bc_sz]
    wrapper_magic = struct.unpack_from("<I", bc_bytes, 0)[0] if len(bc_bytes) >= 20 else 0
    inner_magic = b""
    if wrapper_magic == 0x0B17C0DE:
        _, _, inner_off, _ = struct.unpack_from("<4I", bc_bytes, 0)
        inner_magic = bc_bytes[inner_off : inner_off + 4]
    elif bc_bytes[:4] == b"BC\xc0\xde":
        inner_magic = bc_bytes[:4]

    return {
        "magic": magic,
        "mtlb_ver": v,
        "tags": tags,
        "bc_bytes": bc_bytes,
        "wrapper_magic": wrapper_magic,
        "inner_magic": inner_magic,
    }


def inspect_shader_archive(blob: bytes) -> dict:
    ver, num_shaders = struct.unpack_from("<II", blob, 0)
    pos = 8
    entries = []
    for _ in range(num_shaders):
        sha1 = blob[pos : pos + 20]
        off, comp_sz, uncomp_sz, freq = struct.unpack_from("<QIIB", blob, pos + 20)
        pos += 37
        entries.append((sha1, off, comp_sz, uncomp_sz, freq))

    # Data follows table
    data_base = pos
    stats = {
        "version": ver,
        "count": num_shaders,
        "freq_counts": Counter(),
        "mtlb_count": 0,
        "msl_count": 0,
        "air_2_0_count": 0,
        "wrapper_0b17c0de_count": 0,
        "llvm_bc_count": 0,
        "mtlb_versions": Counter(),
        "tess_count": 0,
        "cnst_count": 0,
        "argbuf_count": 0,
        "raytracing_count": 0,
        "tile_count": 0,
        "air_sample_count": 0,
        "half_count": 0,
        "texturecube_count": 0,
        "read_write_count": 0,
        "threadgroup_count": 0,
        "texture3d_count": 0,
        "texture2d_array_count": 0,
    }

    for _, off, comp_sz, uncomp_sz, freq in entries:
        stats["freq_counts"][freq] += 1
        raw_comp = blob[data_base + off : data_base + off + comp_sz]
        raw = zlib.decompress(raw_comp) if comp_sz != uncomp_sz else raw_comp
        mtlb_idx = raw.find(b"MTLB")
        if mtlb_idx != -1:
            stats["mtlb_count"] += 1
            info = parse_mtlb_tags(raw[mtlb_idx:])
            stats["mtlb_versions"][str(info["mtlb_ver"])] += 1
            tags = info["tags"]
            if "VERS" in tags and len(tags["VERS"]) >= 8:
                vers = struct.unpack_from("<4H", tags["VERS"], 0)
                if vers[:2] == (2, 0):
                    stats["air_2_0_count"] += 1
            if "TESS" in tags:
                stats["tess_count"] += 1
            if "CNST" in tags:
                stats["cnst_count"] += 1
            if info["wrapper_magic"] == 0x0B17C0DE:
                stats["wrapper_0b17c0de_count"] += 1
            if info["inner_magic"] == b"BC\xc0\xde":
                stats["llvm_bc_count"] += 1
            bc = info["bc_bytes"]
            if b"argument_buffer" in bc or b"air.argument" in bc:
                stats["argbuf_count"] += 1
            if b"raytracing" in bc or b"intersection_function" in bc:
                stats["raytracing_count"] += 1
            if b"imageblock" in bc or b"air.imageblock" in bc:
                stats["tile_count"] += 1
            if b"air.sample" in bc:
                stats["air_sample_count"] += 1
            if b"half" in bc:
                stats["half_count"] += 1
            if b"texturecube" in bc:
                stats["texturecube_count"] += 1
            if b"read_write" in bc:
                stats["read_write_count"] += 1
            if b"threadgroup" in bc:
                stats["threadgroup_count"] += 1
            if b"texture3d" in bc:
                stats["texture3d_count"] += 1
            if b"texture2d_array" in bc:
                stats["texture2d_array_count"] += 1
        if b"#include <metal_stdlib>" in raw or b"using namespace metal" in raw:
            stats["msl_count"] += 1

    stats["freq_counts"] = dict(stats["freq_counts"])
    stats["mtlb_versions"] = dict(stats["mtlb_versions"])
    return stats


def inspect_pak_and_shaders(pak_path: str) -> dict:
    header, entries = parse_pak_v8(pak_path)
    ext_stats = defaultdict(lambda: {"count": 0, "comp_bytes": 0, "uncomp_bytes": 0})
    comp_counts = Counter()
    encrypted_entries = 0
    pf_counts = Counter()
    audio_samples = []
    configs = {}
    shader_archives = {}

    with open(pak_path, "rb") as f:
        for e in entries:
            comp_counts[e["comp_method"]] += 1
            if e["flags"] & 1:
                encrypted_entries += 1
            _, ext = os.path.splitext(e["name"])
            ext = ext.lower() or "(no_ext)"
            ext_stats[ext]["count"] += 1
            ext_stats[ext]["comp_bytes"] += e["size"]
            ext_stats[ext]["uncomp_bytes"] += e["uncompressed_size"]

            if ext == ".uasset":
                blob = extract_entry(f, e)
                for pf in PF_NAMES:
                    if pf in blob:
                        pf_counts[pf.decode("ascii")] += 1
            elif e["name"] in (
                "OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode",
                "OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode",
            ):
                blob = extract_entry(f, e)
                shader_archives[e["name"]] = inspect_shader_archive(blob)
            elif e["name"].endswith(("DefaultEngine.ini", "IOSEngine.ini")):
                blob = extract_entry(f, e)
                configs[e["name"]] = blob.decode("utf-8", errors="replace")
            elif e["name"].startswith("OH2/Content/Audio/") and ext in (".uexp", ".ubulk") and len(audio_samples) < 10:
                blob = extract_entry(f, e)
                idx = blob.find(b"RIFF")
                if idx != -1 and blob[idx + 8 : idx + 12] == b"WAVE":
                    fmt_idx = blob.find(b"fmt ", idx)
                    if fmt_idx != -1 and fmt_idx + 16 <= len(blob):
                        wfmt, nch, srate = struct.unpack_from("<HHI", blob, fmt_idx + 8)
                        audio_samples.append({
                            "name": e["name"],
                            "wFormatTag": f"0x{wfmt:04x}",
                            "is_ms_adpcm": wfmt == 0x0002,
                            "channels": nch,
                            "sample_rate": srate,
                        })

    # Extract key INI lines
    ini_highlights = {}
    for k, text in configs.items():
        lines = [
            ln.strip()
            for ln in text.splitlines()
            if any(key in ln for key in ("bCookASTCTextures", "bCookPVRTCTextures", "r.Streaming.PoolSize", "AudioDeviceModuleName", "AudioSampleRate", "MasterEnable"))
        ]
        ini_highlights[k] = lines

    return {
        "pak_header": header,
        "encrypted_entries": encrypted_entries,
        "compression_counts": dict(comp_counts),
        "total_uncompressed_bytes": sum(e["uncompressed_size"] for e in entries),
        "extension_breakdown": {
            k: v for k, v in sorted(ext_stats.items(), key=lambda kv: -kv[1]["uncomp_bytes"])
        },
        "texture_pf_counts": dict(pf_counts.most_common()),
        "audio_samples": audio_samples,
        "ini_highlights": ini_highlights,
        "shader_archives": shader_archives,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect oh2-ios.pak, textures, audio, and shaders")
    parser.add_argument("--pak", default=DEFAULT_PAK)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    res = inspect_pak_and_shaders(args.pak)
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        hdr = res["pak_header"]
        print(
            f"Pak: v{hdr['version']} magic={hdr['magic']} bEncryptedIndex={hdr['bEncryptedIndex']} "
            f"entries={hdr['num_entries']:,} encrypted={res['encrypted_entries']}"
        )
        print(
            f"Compression: zlib={res['compression_counts'].get(1, 0):,}, "
            f"none={res['compression_counts'].get(0, 0):,}, "
            f"uncomp_total={res['total_uncompressed_bytes'] / (1024*1024):.2f} MB"
        )
        print(f"Texture PF_* counts: {res['texture_pf_counts']}")
        print(f"Audio RIFF/WAVE samples checked: {len(res['audio_samples'])} ({sum(1 for a in res['audio_samples'] if a['is_ms_adpcm'])} MS-ADPCM 0x0002)")
        total_shaders = 0
        total_mtlb = 0
        total_msl = 0
        total_bc = 0
        for name, st in res["shader_archives"].items():
            total_shaders += st["count"]
            total_mtlb += st["mtlb_count"]
            total_msl += st["msl_count"]
            total_bc += st["llvm_bc_count"]
            print(
                f"  {os.path.basename(name)}: {st['count']} shaders | freqs={st['freq_counts']} | "
                f"MTLB={st['mtlb_count']} | AIR2.0={st['air_2_0_count']} | BC={st['llvm_bc_count']} | MSL={st['msl_count']} | "
                f"TESS={st['tess_count']} CNST={st['cnst_count']} RT={st['raytracing_count']} TILE={st['tile_count']} ARGBUF={st['argbuf_count']}"
            )
        print(f"Total shaders: {total_shaders:,} (MTLB={total_mtlb:,}, BC={total_bc:,}, MSL={total_msl})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
