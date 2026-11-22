#!/usr/bin/env python3
"""Static Objective-C selector extractor & registration auditor for Payload/OH2.app/OH2."""

from __future__ import annotations

import os
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OH2_BIN = ROOT / "Payload" / "OH2.app" / "OH2"
SRC_DIRS = [ROOT / "src" / "metal_vk", ROOT / "src" / "runtime", ROOT / "src" / "loader"]
OUT_PATH = ROOT / "build" / "m5" / "selectors.txt"

FRAMEWORK_CLASS_PREFIXES = ("MTL", "CAMetalLayer", "UI", "CA", "NS", "EAGL", "GC", "AV")

KEY_AUDIT_CLASSES = [
    "MTLDevice",
    "MTLCommandQueue",
    "MTLCommandBuffer",
    "MTLBlitCommandEncoder",
    "MTLRenderCommandEncoder",
    "MTLComputeCommandEncoder",
    "MTLTexture",
    "MTLBuffer",
    "MTLTextureDescriptor",
    "MTLVertexDescriptor",
    "MTLVertexBufferLayoutDescriptor",
    "MTLVertexAttributeDescriptor",
    "MTLRenderPassDescriptor",
    "MTLRenderPipelineDescriptor",
    "MTLComputePipelineDescriptor",
    "MTLSamplerDescriptor",
    "MTLSamplerState",
    "MTLDepthStencilDescriptor",
    "MTLStencilDescriptor",
    "MTLLibrary",
    "MTLFunction",
    "MTLCompileOptions",
    "MTLFunctionConstantValues",
    "MTLHeap",
    "MTLFence",
    "CAMetalLayer",
    "CAMetalDrawable",
    "CADisplayLink",
    "NSCondition",
    "NSRunLoop",
]


def parse_macho(data: bytes) -> dict:
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != 0xFEEDFACF:
        raise ValueError(f"Unsupported Mach-O magic: 0x{magic:08x}")
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off = 32
    segs: list[tuple[str, int, int, int, int]] = []
    sects: dict[tuple[str, str], tuple[int, int, int]] = {}
    symoff = nsyms = stroff = strsize = 0

    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[off + 8 : off + 24].split(b"\x00", 1)[0].decode("ascii", "replace")
            vm, vs, fo, fs, _, _, nsects = struct.unpack_from("<QQQQIII", data, off + 24)
            segs.append((segname, vm, vs, fo, fs))
            soff = off + 72
            for _ in range(nsects):
                sname = data[soff : soff + 16].split(b"\x00", 1)[0].decode("ascii", "replace")
                sseg = data[soff + 16 : soff + 32].split(b"\x00", 1)[0].decode("ascii", "replace")
                addr, size, sfo = struct.unpack_from("<QQI", data, soff + 32)
                sects[(sseg, sname)] = (addr, size, sfo)
                soff += 80
        elif cmd == 0x02:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
        off += cmdsize

    def vm_to_off(vm: int) -> int | None:
        for _, s_vm, s_vs, s_fo, s_fs in segs:
            if s_vm <= vm < s_vm + s_vs:
                rel = vm - s_vm
                return (s_fo + rel) if rel < s_fs else None
        return None

    def read_cstr_at_off(fo: int) -> str:
        end = data.index(b"\x00", fo)
        return data[fo:end].decode("ascii", "replace")

    selrefs_ordered: list[str] = []
    selrefs_seen: set[str] = set()
    if ("__DATA", "__objc_selrefs") in sects:
        _, size, sfo = sects[("__DATA", "__objc_selrefs")]
        for i in range(0, size, 8):
            ptr = struct.unpack_from("<Q", data, sfo + i)[0]
            pfo = vm_to_off(ptr)
            if pfo is not None:
                name = read_cstr_at_off(pfo)
                if name not in selrefs_seen:
                    selrefs_seen.add(name)
                    selrefs_ordered.append(name)

    methnames: list[str] = []
    if ("__TEXT", "__objc_methname") in sects:
        _, size, sfo = sects[("__TEXT", "__objc_methname")]
        raw = data[sfo : sfo + size]
        methnames = [s.decode("ascii", "replace") for s in raw.split(b"\x00") if s]

    ext_classes: set[str] = set()
    app_classes: set[str] = set()
    if symoff and nsyms and stroff:
        strtab = data[stroff : stroff + strsize]
        for i in range(nsyms):
            strx, n_type = struct.unpack_from("<IB", data, symoff + i * 16)
            if strx >= len(strtab):
                continue
            end = strtab.index(b"\x00", strx) if b"\x00" in strtab[strx:] else len(strtab)
            sym = strtab[strx:end].decode("ascii", "replace")
            if sym.startswith("_OBJC_CLASS_$_"):
                cls_name = sym[len("_OBJC_CLASS_$_") :]
                if (n_type & 0x0E) == 0x00:  # N_UNDF
                    ext_classes.add(cls_name)
                else:
                    app_classes.add(cls_name)

    return {
        "selrefs": selrefs_ordered,
        "methnames": methnames,
        "ext_classes": sorted(ext_classes),
        "app_classes": sorted(app_classes),
    }


def scan_registered_selectors() -> tuple[set[str], dict[str, set[str]], dict[str, set[str]]]:
    inst_by_cls: dict[str, set[str]] = {}
    cls_by_cls: dict[str, set[str]] = {}
    all_sels: set[str] = set()

    pat_named = re.compile(
        r'oh2_objc_register_(instance|class)_method\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"'
    )
    pat_sym = re.compile(
        r'register_(instance|class)_method\s*\(\s*&_OBJC_CLASS_\$_([A-Za-z0-9_]+)\s*,\s*"([^"]+)"'
    )

    for sdir in SRC_DIRS:
        if not sdir.exists():
            continue
        for p in sorted(sdir.rglob("*")):
            if p.suffix not in (".c", ".cc", ".cpp", ".h", ".hpp"):
                continue
            txt = p.read_text(encoding="utf-8", errors="replace")
            for pat in (pat_named, pat_sym):
                for m in pat.finditer(txt):
                    kind, cls_name, sel_name = m.group(1), m.group(2), m.group(3)
                    all_sels.add(sel_name)
                    target = inst_by_cls if kind == "instance" else cls_by_cls
                    target.setdefault(cls_name, set()).add(sel_name)

    return all_sels, inst_by_cls, cls_by_cls


def categorize_selector(sel: str) -> str:
    lo = sel.lower()
    if any(
        k in lo
        for k in (
            "buffer",
            "texture",
            "pipeline",
            "render",
            "compute",
            "blit",
            "sampler",
            "fence",
            "heap",
            "library",
            "function",
            "depth",
            "stencil",
            "viewport",
            "scissor",
            "cull",
            "winding",
            "triangle",
            "primitive",
            "dispatch",
            "threadgroup",
            "encoding",
            "command",
            "capture",
            "argument",
            "drawable",
            "metallayer",
            "cpucache",
            "storagemode",
            "resourceoptions",
            "tile",
            "raster",
            "mipmap",
            "vertex",
            "fragment",
            "blend",
            "pixel",
            "mutability",
            "fastmath",
            "languageversion",
            "constantvalue",
            "iosurface",
            "visibility",
            "stepfunction",
            "steprate",
            "bufferindex",
            "bytesper",
            "allocatedsize",
            "newdefaultlibrary",
        )
    ):
        return "Metal / MetalPP"
    if any(k in lo for k in ("displaylink", "layer", "ca", "sublayer", "contentsscale", "affine", "transform")):
        return "CA / CoreAnimation"
    if any(
        k in lo
        for k in (
            "view",
            "window",
            "screen",
            "touch",
            "gesture",
            "responder",
            "alert",
            "trait",
            "status",
            "keyboard",
            "orientation",
            "brightness",
            "application",
            "haptic",
            "impact",
            "device",
            "pasteboard",
            "safearea",
        )
    ):
        return "UIKit"
    if any(k in lo for k in ("avaudio", "audio", "sound", "player", "session", "category", "sample", "channel", "route")):
        return "AVFoundation / Audio"
    if any(k in lo for k in ("controller", "gamepad", "microgamepad", "dpad", "thumbstick", "trigger", "button")):
        return "GameController (GC)"
    if any(k in lo for k in ("eagl", "gl", "context", "presentrenderbuffer")):
        return "EAGL"
    if any(
        k in lo
        for k in (
            "condition",
            "lock",
            "wait",
            "signal",
            "broadcast",
            "runloop",
            "thread",
            "timer",
            "date",
            "bundle",
            "string",
            "array",
            "dictionary",
            "set",
            "data",
            "number",
            "error",
            "notification",
            "file",
            "directory",
            "url",
            "process",
            "locale",
            "json",
            "propertylist",
            "exception",
            "userdefaults",
            "keyed",
            "invocation",
            "selector",
        )
    ):
        return "Foundation / NS"
    return "Other (App / GameCenter / CrashReporter / Internal)"


def main() -> None:
    data = OH2_BIN.read_bytes()
    info = parse_macho(data)
    all_reg, inst_by_cls, cls_by_cls = scan_registered_selectors()

    fw_ext_classes = [
        c for c in info["ext_classes"] if c.startswith(FRAMEWORK_CLASS_PREFIXES)
    ]

    selrefs = info["selrefs"]
    missing = [s for s in selrefs if s not in all_reg]

    grouped: dict[str, list[tuple[int, str]]] = {}
    for idx, sel in enumerate(selrefs):
        if sel not in all_reg:
            cat = categorize_selector(sel)
            grouped.setdefault(cat, []).append((idx, sel))

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("# Static Objective-C Selector Audit for Payload/OH2.app/OH2")
    lines.append(f"# Total unique selectors in __DATA,__objc_selrefs : {len(selrefs)}")
    lines.append(f"# Total strings in __TEXT,__objc_methname         : {len(info['methnames'])}")
    lines.append(f"# Total registered unique selectors in src/      : {len(all_reg)}")
    lines.append(f"# Missing from __objc_selrefs globally           : {len(missing)}")
    lines.append("")

    lines.append(f"## Referenced Apple Framework Classes ({len(fw_ext_classes)} classes matching prefixes)")
    for c in fw_ext_classes:
        reg_i = len(inst_by_cls.get(c, ()))
        reg_c = len(cls_by_cls.get(c, ()))
        lines.append(f"  - {c} (registered: inst={reg_i}, cls={reg_c})")
    lines.append("")

    lines.append("## Registered Methods Count on Key Audit Classes")
    for c in KEY_AUDIT_CLASSES:
        sels_i = sorted(inst_by_cls.get(c, ()))
        sels_c = sorted(cls_by_cls.get(c, ()))
        lines.append(f"  [{c}] inst={len(sels_i)}, cls={len(sels_c)}")
        if sels_i:
            lines.append(f"    inst: {', '.join(sels_i)}")
        if sels_c:
            lines.append(f"    cls : {', '.join(sels_c)}")
    lines.append("")

    cat_order = [
        "Metal / MetalPP",
        "CA / CoreAnimation",
        "UIKit",
        "Foundation / NS",
        "AVFoundation / Audio",
        "GameController (GC)",
        "EAGL",
        "Other (App / GameCenter / CrashReporter / Internal)",
    ]
    lines.append("## Missing Selectors from __DATA,__objc_selrefs by Category (preserving binary index)")
    for cat in cat_order:
        items = grouped.get(cat, [])
        lines.append("")
        lines.append(f"### {cat} ({len(items)})")
        for idx, sel in items:
            lines.append(f"  [{idx:4d}] {sel}")

    # Also record selectors present in __objc_methname starting with common Metal/UI prefixes not in selrefs or reg
    meth_set = set(info["methnames"]) - set(selrefs) - all_reg
    mtl_extra_meths = sorted(
        s for s in meth_set if categorize_selector(s) in ("Metal / MetalPP", "CA / CoreAnimation")
    )
    lines.append("")
    lines.append(f"## Additional Metal/CA strings in __TEXT,__objc_methname not in __objc_selrefs or src/ ({len(mtl_extra_meths)})")
    for s in mtl_extra_meths:
        lines.append(f"  {s}")

    OUT_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {OUT_PATH}")
    print(
        f"Summary: selrefs={len(selrefs)}, methnames={len(info['methnames'])}, "
        f"reg={len(all_reg)}, missing_selrefs={len(missing)}, fw_classes={len(fw_ext_classes)}"
    )
    for cat in cat_order:
        print(f"  {cat}: {len(grouped.get(cat, []))}")


if __name__ == "__main__":
    main()
