#!/usr/bin/env python3
"""
Reproducible M0 Objective-C metadata inspection for Payload/OH2.app/OH2.
Verifies ObjC sections, 60 classes, 2 categories, 28 protocols, 1,491 selectors,
192 classrefs (134 external across 16 frameworks), and 52 superrefs.
"""

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

DEFAULT_BINARY = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "Payload",
    "OH2.app",
    "OH2",
)


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


def parse_macho_layout(data: bytes):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<8I", data, 0)
    assert magic == 0xFEEDFACF
    segments = []
    sections = {}
    dylibs = []
    dyld_info = None
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:
            segname, vmaddr, vmsize, fileoff, filesize, _, _, nsects, _ = struct.unpack_from(
                "<16sQQQQIIII", data, off + 8
            )
            seg_s = segname.split(b"\x00", 1)[0].decode("ascii")
            segments.append((seg_s, vmaddr, vmsize, fileoff, filesize))
            so = off + 72
            for _ in range(nsects):
                sectname, s_segname, addr, size, s_off, align, _, _, flags, _, _, _ = struct.unpack_from(
                    "<16s16sQQIIIIIIII", data, so
                )
                sn = sectname.split(b"\x00", 1)[0].decode("ascii")
                sgn = s_segname.split(b"\x00", 1)[0].decode("ascii")
                sections[(sgn, sn)] = {"addr": addr, "size": size, "offset": s_off, "flags": flags}
                so += 80
        elif cmd in (0x0C, 0x80000018):
            name_off = struct.unpack_from("<I", data, off + 8)[0]
            p, _ = read_cstr(data, off + name_off)
            dylibs.append(p)
        elif cmd in (0x22, 0x80000022):
            vals = struct.unpack_from("<10I", data, off + 8)
            dyld_info = {"bind_off": vals[2], "bind_size": vals[3]}
        off += cmdsize
    return segments, sections, dylibs, dyld_info


def vm_to_off(segments, vmaddr: int) -> int | None:
    if vmaddr == 0:
        return None
    for _, vma, vms, foff, fsz in segments:
        if vma <= vmaddr < vma + fsz:
            return foff + (vmaddr - vma)
    return None


def decode_bind_map(data: bytes, segments, dylibs: list[str], bind_off: int, bind_size: int) -> dict[int, tuple[str, str]]:
    stream = data[bind_off : bind_off + bind_size]
    pos = 0
    end = len(stream)
    ordinal = 0
    sym_name = ""
    seg_idx = 0
    seg_off = 0
    vm_to_sym: dict[int, tuple[str, str]] = {}

    def record():
        if 0 <= seg_idx < len(segments):
            vm = segments[seg_idx][1] + seg_off
            lib = dylibs[ordinal - 1] if 1 <= ordinal <= len(dylibs) else f"ORD_{ordinal}"
            vm_to_sym[vm] = (lib, sym_name)

    while pos < end:
        op = stream[pos]
        pos += 1
        opcode = op & 0xF0
        imm = op & 0x0F
        if opcode == 0x00:
            break
        elif opcode == 0x10:
            ordinal = imm
        elif opcode == 0x20:
            ordinal, pos = read_uleb128(stream, pos)
        elif opcode == 0x30:
            ordinal = imm - 16 if imm else 0
        elif opcode == 0x40:
            sym_name, pos = read_cstr(stream, pos)
        elif opcode == 0x50:
            pass
        elif opcode == 0x60:
            _, pos = read_sleb128(stream, pos)
        elif opcode == 0x70:
            seg_idx = imm
            seg_off, pos = read_uleb128(stream, pos)
        elif opcode == 0x80:
            delta, pos = read_uleb128(stream, pos)
            seg_off = (seg_off + delta) & 0xFFFFFFFFFFFFFFFF
        elif opcode == 0x90:
            record()
            seg_off += 8
        elif opcode == 0xA0:
            record()
            delta, pos = read_uleb128(stream, pos)
            seg_off = (seg_off + 8 + delta) & 0xFFFFFFFFFFFFFFFF
        elif opcode == 0xB0:
            record()
            seg_off += 8 + imm * 8
        elif opcode == 0xC0:
            count, pos = read_uleb128(stream, pos)
            skip, pos = read_uleb128(stream, pos)
            for _ in range(count):
                record()
                seg_off += 8 + skip
        else:
            raise ValueError(f"Unsupported bind opcode 0x{op:02x}")
    return vm_to_sym


def inspect_objc(binary_path: str) -> dict:
    with open(binary_path, "rb") as f:
        data = f.read()

    segments, sections, dylibs, dyld_info = parse_macho_layout(data)
    bind_map = decode_bind_map(data, segments, dylibs, dyld_info["bind_off"], dyld_info["bind_size"])

    objc_sections = {}
    for (seg, sec), info in sections.items():
        if "objc" in sec:
            objc_sections[f"{seg},{sec}"] = {"addr": f"0x{info['addr']:x}", "size": info["size"]}

    def read_ptr_array(sec_key):
        s = sections[sec_key]
        n = s["size"] // 8
        return list(struct.unpack_from(f"<{n}Q", data, s["offset"])), s["addr"]

    # 1. Classes (__objc_classlist)
    class_ptrs, _ = read_ptr_array(("__DATA", "__objc_classlist"))
    classes = []
    class_vm_to_name = {}
    for c_vm in class_ptrs:
        c_off = vm_to_off(segments, c_vm)
        isa, superclass, cache, vtable, data_ptr = struct.unpack_from("<5Q", data, c_off)
        ro_vm = data_ptr & ~0x7
        ro_off = vm_to_off(segments, ro_vm)
        # class_ro_t on arm64: flags(4), instanceStart(4), instanceSize(4), reserved(4), ivarLayout(8), name(8), baseMethodList(8), ...
        _, _, inst_size, _, _, name_vm, mlist_vm, plist_vm, ilist_vm, _, _ = struct.unpack_from(
            "<4I7Q", data, ro_off
        )
        name_off = vm_to_off(segments, name_vm)
        cname, _ = read_cstr(data, name_off)
        class_vm_to_name[c_vm] = cname
        super_name = None
        if superclass in bind_map:
            super_name = bind_map[superclass][1]
        elif superclass in class_vm_to_name:
            super_name = class_vm_to_name[superclass]
        elif superclass != 0:
            # Might be defined later in classlist; resolve in second pass
            super_name = f"VM_0x{superclass:x}"
        classes.append({"name": cname, "vmaddr": f"0x{c_vm:x}", "super_vm": superclass, "super": super_name, "instance_size": inst_size})

    # Resolve local superclasses defined later in __objc_classlist
    for c in classes:
        svm = c.pop("super_vm")
        if svm in class_vm_to_name:
            c["super"] = class_vm_to_name[svm]

    # 2. Categories (__objc_catlist)
    cat_ptrs, _ = read_ptr_array(("__DATA", "__objc_catlist"))
    categories = []
    for cat_vm in cat_ptrs:
        cat_off = vm_to_off(segments, cat_vm)
        name_vm, cls_vm = struct.unpack_from("<2Q", data, cat_off)
        cname, _ = read_cstr(data, vm_to_off(segments, name_vm))
        if (cat_vm + 8) in bind_map:
            target = bind_map[cat_vm + 8][1].replace("_OBJC_CLASS_$_", "")
        elif cls_vm in bind_map:
            target = bind_map[cls_vm][1].replace("_OBJC_CLASS_$_", "")
        elif cls_vm in class_vm_to_name:
            target = class_vm_to_name[cls_vm]
        else:
            target = f"0x{cls_vm:x}"
        categories.append(f"{target} ({cname})")

    # 3. Protocols (__objc_protolist)
    proto_ptrs, _ = read_ptr_array(("__DATA", "__objc_protolist"))
    protocols = []
    for p_vm in proto_ptrs:
        p_off = vm_to_off(segments, p_vm)
        _, name_vm = struct.unpack_from("<2Q", data, p_off)
        pname, _ = read_cstr(data, vm_to_off(segments, name_vm))
        protocols.append(pname)

    # 4. Selectors (__objc_selrefs)
    sel_ptrs, _ = read_ptr_array(("__DATA", "__objc_selrefs"))
    selectors = []
    for s_vm in sel_ptrs:
        s_off = vm_to_off(segments, s_vm)
        sname, _ = read_cstr(data, s_off)
        selectors.append(sname)

    # 5. Classrefs (__objc_classrefs)
    cref_ptrs, cref_base_vm = read_ptr_array(("__DATA", "__objc_classrefs"))
    external_by_fw = defaultdict(list)
    local_classrefs = []
    for idx, c_vm in enumerate(cref_ptrs):
        slot_vm = cref_base_vm + idx * 8
        if slot_vm in bind_map:
            lib, sym = bind_map[slot_vm]
            fw = os.path.basename(lib).replace(".framework", "")
            external_by_fw[fw].append(sym.replace("_OBJC_CLASS_$_", ""))
        elif c_vm in class_vm_to_name:
            local_classrefs.append(class_vm_to_name[c_vm])
        else:
            local_classrefs.append(f"0x{c_vm:x}")

    # 6. Superrefs (__objc_superrefs)
    sref_ptrs, sref_base_vm = read_ptr_array(("__DATA", "__objc_superrefs"))
    superrefs = []
    for idx, s_vm in enumerate(sref_ptrs):
        slot_vm = sref_base_vm + idx * 8
        if slot_vm in bind_map:
            superrefs.append(bind_map[slot_vm][1])
        elif s_vm in class_vm_to_name:
            superrefs.append(class_vm_to_name[s_vm])
        else:
            superrefs.append(f"0x{s_vm:x}")

    return {
        "objc_sections": objc_sections,
        "class_count": len(classes),
        "classes": classes,
        "category_count": len(categories),
        "categories": categories,
        "protocol_count": len(protocols),
        "protocols": protocols,
        "selref_count": len(selectors),
        "selref_unique_count": len(set(selectors)),
        "classref_count": len(cref_ptrs),
        "classref_external_count": sum(len(v) for v in external_by_fw.values()),
        "classref_local_count": len(local_classrefs),
        "external_frameworks_count": len(external_by_fw),
        "external_by_framework": {k: sorted(v) for k, v in sorted(external_by_fw.items(), key=lambda kv: -len(kv[1]))},
        "superref_count": len(superrefs),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect Objective-C metadata in OH2")
    parser.add_argument("--binary", default=DEFAULT_BINARY)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    res = inspect_objc(args.binary)
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print(f"ObjC sections: {len(res['objc_sections'])}")
        print(f"Classes: {res['class_count']}, Categories: {res['category_count']}, Protocols: {res['protocol_count']}")
        print(f"Categories: {res['categories']}")
        print(f"Selectors (__objc_selrefs): {res['selref_count']} ({res['selref_unique_count']} unique)")
        print(
            f"Classrefs: {res['classref_count']} ({res['classref_external_count']} external across "
            f"{res['external_frameworks_count']} frameworks, {res['classref_local_count']} local)"
        )
        for fw, cls_list in res["external_by_framework"].items():
            print(f"  - {fw}: {len(cls_list)} ({', '.join(cls_list[:5])}{'...' if len(cls_list) > 5 else ''})")
        print(f"Superrefs: {res['superref_count']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
