#!/usr/bin/env python3
"""
Reproducible M0 Mach-O binary inspection for Payload/OH2.app/OH2.
Verifies header, segments/sections, LC_ENCRYPTION_INFO_64 (cryptid == 0),
LC_MAIN ARM64 prologue, LC_DYLD_INFO_ONLY counts, __mod_init_func,
LC_LOAD*_DYLIB, undefined symbols by dylib, libc++ std::__1, TLS MRS scan,
__unwind_info / __eh_frame, and Darwin variadic / va_list imports.
"""

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from macholib.MachO import MachO
import numpy as np

DEFAULT_BINARY = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "Payload",
    "OH2.app",
    "OH2",
)

VARIADIC_IMPORTS = [
    "_printf",
    "_fprintf",
    "_sprintf",
    "_snprintf",
    "___sprintf_chk",
    "___snprintf_chk",
    "_sscanf",
    "_NSLog",
    "_open",
    "_fcntl",
    "_ioctl",
    "_syscall",
]

VA_LIST_IMPORTS = [
    "_vfprintf",
    "_vsnprintf",
]


def read_uleb128(data: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            break
        shift += 7
    return result, pos


def read_sleb128(data: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if (b & 0x80) == 0:
            if b & 0x40:
                result |= -(1 << shift)
            break
    return result, pos


def read_cstr(data: bytes, pos: int) -> tuple[str, int]:
    end = data.index(0, pos)
    return data[pos:end].decode("utf-8", errors="replace"), end + 1


def count_rebase_opcodes(stream: bytes) -> int:
    pos = 0
    end = len(stream)
    count = 0
    ptr_size = 8
    while pos < end:
        op = stream[pos]
        pos += 1
        opcode = op & 0xF0
        imm = op & 0x0F
        if opcode == 0x00:  # REBASE_OPCODE_DONE
            break
        elif opcode in (0x10, 0x20):  # SET_TYPE_IMM / SET_SEGMENT_AND_OFFSET_ULEB
            if opcode == 0x20:
                _, pos = read_uleb128(stream, pos)
        elif opcode == 0x30:  # ADD_ADDR_ULEB
            _, pos = read_uleb128(stream, pos)
        elif opcode == 0x40:  # ADD_ADDR_IMM_SCALED
            pass
        elif opcode == 0x50:  # DO_REBASE_IMM_TIMES
            count += imm
        elif opcode == 0x60:  # DO_REBASE_ULEB_TIMES
            n, pos = read_uleb128(stream, pos)
            count += n
        elif opcode == 0x70:  # DO_REBASE_ADD_ADDR_ULEB
            count += 1
            _, pos = read_uleb128(stream, pos)
        elif opcode == 0x80:  # DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
            n, pos = read_uleb128(stream, pos)
            _, pos = read_uleb128(stream, pos)
            count += n
        else:
            raise ValueError(f"Unknown rebase opcode 0x{op:02x} at {pos - 1}")
    return count


def parse_bind_opcodes(stream: bytes, is_lazy: bool = False) -> tuple[int, set[tuple[int, str]]]:
    pos = 0
    end = len(stream)
    ordinal = 0
    sym_name = ""
    sites = 0
    unique = set()
    while pos < end:
        op = stream[pos]
        pos += 1
        opcode = op & 0xF0
        imm = op & 0x0F
        if opcode == 0x00:  # DONE
            if is_lazy:
                # lazy bind has a DONE at the end of each stub entry
                continue
            break
        elif opcode == 0x10:  # SET_DYLIB_ORDINAL_IMM
            ordinal = imm
        elif opcode == 0x20:  # SET_DYLIB_ORDINAL_ULEB
            ordinal, pos = read_uleb128(stream, pos)
        elif opcode == 0x30:  # SET_DYLIB_SPECIAL_IMM
            ordinal = imm - 16 if imm else 0
        elif opcode == 0x40:  # SET_SYMBOL_TRAILING_FLAGS_IMM
            sym_name, pos = read_cstr(stream, pos)
        elif opcode == 0x50:  # SET_TYPE_IMM
            pass
        elif opcode == 0x60:  # SET_ADDEND_SLEB
            _, pos = read_sleb128(stream, pos)
        elif opcode == 0x70:  # SET_SEGMENT_AND_OFFSET_ULEB
            _, pos = read_uleb128(stream, pos)
        elif opcode == 0x80:  # ADD_ADDR_ULEB
            _, pos = read_uleb128(stream, pos)
        elif opcode == 0x90:  # DO_BIND
            sites += 1
            unique.add((ordinal, sym_name))
        elif opcode == 0xA0:  # DO_BIND_ADD_ADDR_ULEB
            sites += 1
            unique.add((ordinal, sym_name))
            _, pos = read_uleb128(stream, pos)
        elif opcode == 0xB0:  # DO_BIND_ADD_ADDR_IMM_SCALED
            sites += 1
            unique.add((ordinal, sym_name))
        elif opcode == 0xC0:  # DO_BIND_ULEB_TIMES_SKIPPING_ULEB
            n, pos = read_uleb128(stream, pos)
            _, pos = read_uleb128(stream, pos)
            sites += n
            unique.add((ordinal, sym_name))
        elif opcode == 0xD0:  # THREADED
            sub = imm
            if sub == 0:
                _, pos = read_uleb128(stream, pos)
            elif sub == 1:
                sites += 1
                unique.add((ordinal, sym_name))
        else:
            raise ValueError(f"Unknown bind opcode 0x{op:02x} at {pos - 1}")
    return sites, unique


def count_export_trie(trie: bytes) -> list[str]:
    exports = []

    def walk(offset: int, prefix: str) -> None:
        term_size, pos = read_uleb128(trie, offset)
        if term_size != 0:
            exports.append(prefix)
            pos += term_size
        else:
            pass
        # Re-read properly: node = uleb128(terminal_size), [terminal_info], uint8 num_children, children...
        t_size, p = read_uleb128(trie, offset)
        if t_size > 0:
            p += t_size
        num_children = trie[p]
        p += 1
        for _ in range(num_children):
            edge, p = read_cstr(trie, p)
            child_off, p = read_uleb128(trie, p)
            walk(child_off, prefix + edge)

    if trie:
        walk(0, "")
    return exports


def inspect_macho(binary_path: str) -> dict:
    file_size = os.path.getsize(binary_path)
    with open(binary_path, "rb") as f:
        data = f.read()

    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from(
        "<8I", data, 0
    )

    m = MachO(binary_path)
    hdr = m.headers[0]

    segments = []
    sections = []
    dylibs = []
    enc_info = None
    main_info = None
    dyld_info = None
    symtab_info = None
    dysymtab_info = None
    build_version = None
    uuid_hex = None
    dylinker = None
    rpaths = []

    off = 32
    for idx in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, segflags = struct.unpack_from(
                "<16sQQQQIIII", data, off + 8
            )
            segname_s = segname.split(b"\x00", 1)[0].decode("ascii")
            segments.append(
                {
                    "name": segname_s,
                    "vmaddr": vmaddr,
                    "vmsize": vmsize,
                    "fileoff": fileoff,
                    "filesize": filesize,
                    "maxprot": maxprot,
                    "initprot": initprot,
                    "nsects": nsects,
                }
            )
            sec_off = off + 72
            for _ in range(nsects):
                sectname, s_segname, addr, size, s_offset, align, reloff, nreloc, s_flags, r1, r2, r3 = struct.unpack_from(
                    "<16s16sQQIIIIIIII", data, sec_off
                )
                sections.append(
                    {
                        "segname": s_segname.split(b"\x00", 1)[0].decode("ascii"),
                        "sectname": sectname.split(b"\x00", 1)[0].decode("ascii"),
                        "addr": addr,
                        "size": size,
                        "offset": s_offset,
                        "align": align,
                        "flags": s_flags,
                        "reserved1": r1,
                        "reserved2": r2,
                    }
                )
                sec_off += 80
        elif cmd in (0x21, 0x2C):  # LC_ENCRYPTION_INFO(_64)
            cryptoff, cryptsize, cryptid = struct.unpack_from("<III", data, off + 8)
            enc_info = {
                "cmd": f"0x{cmd:x}",
                "index": idx,
                "offset": off,
                "cryptoff": cryptoff,
                "cryptsize": cryptsize,
                "cryptid": cryptid,
            }
        elif cmd == 0x80000028:  # LC_MAIN
            entryoff, stacksize = struct.unpack_from("<QQ", data, off + 8)
            main_info = {"entryoff": entryoff, "stacksize": stacksize}
        elif cmd in (0x22, 0x80000022):  # LC_DYLD_INFO(_ONLY)
            (
                rebase_off,
                rebase_size,
                bind_off,
                bind_size,
                weak_off,
                weak_size,
                lazy_off,
                lazy_size,
                export_off,
                export_size,
            ) = struct.unpack_from("<10I", data, off + 8)
            dyld_info = {
                "rebase_off": rebase_off,
                "rebase_size": rebase_size,
                "bind_off": bind_off,
                "bind_size": bind_size,
                "weak_off": weak_off,
                "weak_size": weak_size,
                "lazy_off": lazy_off,
                "lazy_size": lazy_size,
                "export_off": export_off,
                "export_size": export_size,
            }
        elif cmd == 0x02:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", data, off + 8)
            symtab_info = {"symoff": symoff, "nsyms": nsyms, "stroff": stroff, "strsize": strsize}
        elif cmd == 0x0B:  # LC_DYSYMTAB
            vals = struct.unpack_from("<18I", data, off + 8)
            dysymtab_info = {
                "ilocalsym": vals[0],
                "nlocalsym": vals[1],
                "iextdefsym": vals[2],
                "nextdefsym": vals[3],
                "iundefsym": vals[4],
                "nundefsym": vals[5],
                "indirectsymoff": vals[12],
                "nindirectsyms": vals[13],
            }
        elif cmd in (0x0C, 0x80000018):  # LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB
            name_off, ts, cur_ver, compat_ver = struct.unpack_from("<4I", data, off + 8)
            path, _ = read_cstr(data, off + name_off)
            dylibs.append(
                {
                    "ordinal": len(dylibs) + 1,
                    "weak": cmd == 0x80000018,
                    "path": path,
                    "current_version": f"{cur_ver >> 16}.{(cur_ver >> 8) & 0xff}.{cur_ver & 0xff}",
                    "compat_version": f"{compat_ver >> 16}.{(compat_ver >> 8) & 0xff}.{compat_ver & 0xff}",
                }
            )
        elif cmd == 0x32:  # LC_BUILD_VERSION
            platform, minos, sdk, ntools = struct.unpack_from("<4I", data, off + 8)
            build_version = {
                "platform": platform,
                "minos": f"{minos >> 16}.{(minos >> 8) & 0xff}.{minos & 0xff}",
                "sdk": f"{sdk >> 16}.{(sdk >> 8) & 0xff}.{sdk & 0xff}",
                "ntools": ntools,
            }
        elif cmd == 0x1B:  # LC_UUID
            uuid_hex = data[off + 8 : off + 24].hex()
        elif cmd == 0x0E:  # LC_LOAD_DYLINKER
            name_off = struct.unpack_from("<I", data, off + 8)[0]
            dylinker, _ = read_cstr(data, off + name_off)
        elif cmd == 0x8000001C:  # LC_RPATH
            name_off = struct.unpack_from("<I", data, off + 8)[0]
            rp, _ = read_cstr(data, off + name_off)
            rpaths.append(rp)
        off += cmdsize

    # Disassemble entry point prologue (16 instructions)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    entry_bytes = data[main_info["entryoff"] : main_info["entryoff"] + 64]
    prologue = [f"{insn.mnemonic} {insn.op_str}".strip() for insn in md.disasm(entry_bytes, 0x100000000 + main_info["entryoff"])]

    # Parse LC_DYLD_INFO_ONLY streams
    rebase_count = count_rebase_opcodes(
        data[dyld_info["rebase_off"] : dyld_info["rebase_off"] + dyld_info["rebase_size"]]
    )
    bind_sites, bind_uniq = parse_bind_opcodes(
        data[dyld_info["bind_off"] : dyld_info["bind_off"] + dyld_info["bind_size"]]
    )
    weak_sites, weak_uniq = parse_bind_opcodes(
        data[dyld_info["weak_off"] : dyld_info["weak_off"] + dyld_info["weak_size"]]
    )
    lazy_sites, lazy_uniq = parse_bind_opcodes(
        data[dyld_info["lazy_off"] : dyld_info["lazy_off"] + dyld_info["lazy_size"]],
        is_lazy=True,
    )
    exports = count_export_trie(
        data[dyld_info["export_off"] : dyld_info["export_off"] + dyld_info["export_size"]]
    )

    # __mod_init_func
    sec_map = {(s["segname"], s["sectname"]): s for s in sections}
    mod_init = sec_map[("__DATA", "__mod_init_func")]
    mod_init_count = mod_init["size"] // 8
    first_5_inits = list(struct.unpack_from("<5Q", data, mod_init["offset"]))

    # Undefined symbols by dylib ordinal from LC_SYMTAB
    undef_by_dylib = defaultdict(list)
    undef_all = []
    libcxx_syms = []
    sym_base = symtab_info["symoff"]
    str_base = symtab_info["stroff"]
    i0 = dysymtab_info["iundefsym"]
    i1 = i0 + dysymtab_info["nundefsym"]
    for i in range(i0, i1):
        n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from("<IBBHQ", data, sym_base + i * 16)
        name, _ = read_cstr(data, str_base + n_strx)
        lib_ord = (n_desc >> 8) & 0xFF
        dylib_path = dylibs[lib_ord - 1]["path"] if 1 <= lib_ord <= len(dylibs) else f"ORD_{lib_ord}"
        undef_by_dylib[dylib_path].append(name)
        undef_all.append(name)
        if dylib_path == "/usr/lib/libc++.1.dylib":
            libcxx_syms.append(name)

    # TLS / MRS / MSR instruction scan across __TEXT,{__text,__stubs,__stub_helper}
    mrs_tpidrro = 0
    mrs_tpidr = 0
    mrs_any = 0
    msr_any = 0
    words_scanned = 0
    for sname in ("__text", "__stubs", "__stub_helper"):
        sec = sec_map[("__TEXT", sname)]
        nwords = sec["size"] // 4
        words_scanned += nwords
        arr = np.frombuffer(data, dtype="<u4", count=nwords, offset=sec["offset"])
        mrs_tpidrro += int(np.count_nonzero((arr & 0xFFFFFFE0) == 0xD53BD060))
        mrs_tpidr += int(np.count_nonzero((arr & 0xFFFFFFE0) == 0xD53BD040))
        mrs_any += int(np.count_nonzero((arr & 0xFFF00000) == 0xD5300000))
        msr_any += int(np.count_nonzero((arr & 0xFFF00000) == 0xD5100000))

    # Unwind sections
    unwind_sec = sec_map[("__TEXT", "__unwind_info")]
    eh_frame_sec = sec_map[("__TEXT", "__eh_frame")]
    gcc_except_sec = sec_map[("__TEXT", "__gcc_except_tab")]

    # Variadic & va_list check
    undef_set = set(undef_all)
    variadic_found = [s for s in VARIADIC_IMPORTS if s in undef_set]
    valist_found = [s for s in VA_LIST_IMPORTS if s in undef_set]

    return {
        "binary_path": binary_path,
        "file_size": file_size,
        "macholib_cmd_count": len(hdr.commands),
        "header": {
            "magic": f"0x{magic:08x}",
            "cputype": f"0x{cputype:08x}",
            "cpusubtype": f"0x{cpusubtype:08x}",
            "filetype": filetype,
            "ncmds": ncmds,
            "sizeofcmds": sizeofcmds,
            "flags": f"0x{flags:08x}",
            "uuid": uuid_hex,
            "dylinker": dylinker,
            "rpaths": rpaths,
            "build_version": build_version,
        },
        "encryption_info": enc_info,
        "main": main_info,
        "main_prologue_16": prologue,
        "segments": segments,
        "section_count": len(sections),
        "dyld_info_counts": {
            "rebase_pointers": rebase_count,
            "bind_sites": bind_sites,
            "bind_unique_pairs": len(bind_uniq),
            "weak_bind_sites": weak_sites,
            "weak_bind_unique_syms": len({s for _, s in weak_uniq}),
            "lazy_bind_sites": lazy_sites,
            "lazy_bind_unique_pairs": len(lazy_uniq),
            "export_symbols": len(exports),
        },
        "mod_init_func": {
            "count": mod_init_count,
            "first_5": [f"0x{x:x}" for x in first_5_inits],
        },
        "dylib_count": len(dylibs),
        "dylib_weak_count": sum(1 for d in dylibs if d["weak"]),
        "dylibs": dylibs,
        "symtab": symtab_info,
        "dysymtab": dysymtab_info,
        "undefined_symbol_total": len(undef_all),
        "undefined_by_dylib_counts": {k: len(v) for k, v in undef_by_dylib.items()},
        "libcxx_symbols": sorted(libcxx_syms),
        "tls_scan": {
            "words_scanned": words_scanned,
            "has_thread_sections": any(s["sectname"].startswith("__thread") for s in sections),
            "mrs_tpidrro_el0": mrs_tpidrro,
            "mrs_tpidr_el0": mrs_tpidr,
            "mrs_any": mrs_any,
            "msr_any": msr_any,
        },
        "unwind": {
            "unwind_info_size": unwind_sec["size"],
            "eh_frame_size": eh_frame_sec["size"],
            "gcc_except_tab_size": gcc_except_sec["size"],
        },
        "variadic_imports_found": variadic_found,
        "va_list_imports_found": valist_found,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect Payload/OH2.app/OH2 Mach-O binary")
    parser.add_argument("--binary", default=DEFAULT_BINARY)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    res = inspect_macho(args.binary)
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print(f"Binary: {res['binary_path']} ({res['file_size']:,} bytes)")
        print(f"Header: magic={res['header']['magic']} ncmds={res['header']['ncmds']} flags={res['header']['flags']}")
        enc = res["encryption_info"]
        print(f"Encryption: cmd={enc['cmd']} cryptoff=0x{enc['cryptoff']:x} cryptsize=0x{enc['cryptsize']:x} cryptid={enc['cryptid']}")
        print(f"LC_MAIN: entryoff=0x{res['main']['entryoff']:x} | prologue[0..2]={res['main_prologue_16'][:3]}")
        print(f"Segments: {len(res['segments'])}, Sections: {res['section_count']}")
        di = res["dyld_info_counts"]
        print(
            f"LC_DYLD_INFO_ONLY: rebase={di['rebase_pointers']:,}, bind={di['bind_sites']:,} ({di['bind_unique_pairs']} uniq), "
            f"weak={di['weak_bind_sites']} ({di['weak_bind_unique_syms']} uniq), lazy={di['lazy_bind_sites']}, exports={di['export_symbols']}"
        )
        print(f"__mod_init_func: {res['mod_init_func']['count']} pointers")
        print(f"Dylibs: {res['dylib_count']} ({res['dylib_weak_count']} weak), Undefined symbols: {res['undefined_symbol_total']}")
        print(f"libc++.1.dylib symbols: {len(res['libcxx_symbols'])}")
        tls = res["tls_scan"]
        print(
            f"TLS scan ({tls['words_scanned']:,} insns): thread_secs={tls['has_thread_sections']}, "
            f"TPIDRRO_EL0={tls['mrs_tpidrro_el0']}, TPIDR_EL0={tls['mrs_tpidr_el0']}, MRS={tls['mrs_any']}, MSR={tls['msr_any']}"
        )
        uw = res["unwind"]
        print(f"Unwind: __unwind_info={uw['unwind_info_size']} B, __eh_frame={uw['eh_frame_size']} B, __gcc_except_tab={uw['gcc_except_tab_size']} B")
        print(f"Variadic imports ({len(res['variadic_imports_found'])}): {res['variadic_imports_found']}")
        print(f"va_list imports ({len(res['va_list_imports_found'])}): {res['va_list_imports_found']}")

    assert res["encryption_info"]["cryptid"] == 0, "FairPlay cryptid != 0!"
    return 0


if __name__ == "__main__":
    sys.exit(main())
