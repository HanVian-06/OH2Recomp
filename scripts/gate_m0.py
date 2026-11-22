#!/usr/bin/env python3
"""
Repeatable Gate M0:
1. Parses Payload/OH2.app/OH2 (LC_ENCRYPTION_INFO_64) directly to assert cryptid == 0
   (prints hard-stop Human Checkpoint 1 error if != 0 or missing).
2. Verifies all required docs/ and docs/analysis/ files exist, are non-empty,
   and contain verified key metrics.
3. Recursively checks g:\\Projec Porting (excluding .git/) to assert ZERO .md files
   exist anywhere outside docs/.
"""

import os
import struct
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OH2_BINARY = os.path.join(REPO_ROOT, "Payload", "OH2.app", "OH2")
DOCS_DIR = os.path.join(REPO_ROOT, "docs")

REQUIRED_DOCS = [
    "docs/PLAN.md",
    "docs/DECISIONS.md",
    "docs/PROGRESS.md",
    "docs/BLOCKERS.md",
    "docs/analysis/macho_binary.md",
    "docs/analysis/objc_metadata.md",
    "docs/analysis/shaders_air.md",
    "docs/analysis/pak_textures_audio.md",
    "docs/analysis/toolchain_device.md",
]

REQUIRED_METRICS = [
    ("docs/analysis/macho_binary.md", ["53,729,072", "520,119", "805", "cryptid == 0"]),
    ("docs/analysis/objc_metadata.md", ["60", "1,491", "192", "134", "52"]),
    ("docs/analysis/shaders_air.md", ["4,544", "MTLB", "0 / 4,544"]),
    ("docs/analysis/pak_textures_audio.md", ["19,920", "PF_ASTC_8x8", "PF_ASTC_4x4"]),
    ("docs/analysis/toolchain_device.md", ["SM-A566B", "Samsung Xclipse 540"]),
]


def check_macho_cryptid(binary_path: str) -> tuple[bool, str]:
    if not os.path.isfile(binary_path):
        return False, f"Binary not found: {binary_path}"
    with open(binary_path, "rb") as f:
        header = f.read(32)
        if len(header) < 32:
            return False, "File too small for mach_header_64"
        magic, _, _, _, ncmds, sizeofcmds, _, _ = struct.unpack_from("<8I", header, 0)
        if magic != 0xFEEDFACF:
            return False, f"Unexpected Mach-O magic 0x{magic:08x} (expected 0xfeedfacf)"
        cmds_blob = f.read(sizeofcmds)

    off = 0
    for idx in range(ncmds):
        if off + 8 > len(cmds_blob):
            return False, "Truncated Mach-O load commands"
        cmd, cmdsize = struct.unpack_from("<II", cmds_blob, off)
        if cmd in (0x21, 0x2C):  # LC_ENCRYPTION_INFO / LC_ENCRYPTION_INFO_64
            cryptoff, cryptsize, cryptid = struct.unpack_from("<III", cmds_blob, off + 8)
            if cryptid != 0:
                return (
                    False,
                    f"HUMAN CHECKPOINT 1 HARD STOP: FairPlay encrypted (cryptid={cryptid}, "
                    f"cryptoff=0x{cryptoff:x}, cryptsize=0x{cryptsize:x}). Provide decrypted dump.",
                )
            return (
                True,
                f"LC_ENCRYPTION_INFO_64 (cmd=0x{cmd:x}, idx={idx}): "
                f"cryptoff=0x{cryptoff:x}, cryptsize=0x{cryptsize:x}, cryptid=0",
            )
        off += cmdsize
    return False, "LC_ENCRYPTION_INFO(_64) load command not found"


def check_docs() -> list[tuple[str, bool, str]]:
    results = []
    for rel in REQUIRED_DOCS:
        full = os.path.join(REPO_ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(full):
            results.append((f"Doc exists: {rel}", False, "MISSING"))
        elif os.path.getsize(full) == 0:
            results.append((f"Doc non-empty: {rel}", False, "EMPTY"))
        else:
            results.append((f"Doc exists: {rel}", True, f"{os.path.getsize(full):,} bytes"))

    for rel, needles in REQUIRED_METRICS:
        full = os.path.join(REPO_ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(full):
            results.append((f"Metrics in {rel}", False, "file missing"))
            continue
        with open(full, "r", encoding="utf-8") as f:
            text = f.read()
        missing = [n for n in needles if n not in text]
        if missing:
            results.append((f"Metrics in {rel}", False, f"Missing tokens: {missing}"))
        else:
            results.append((f"Metrics in {rel}", True, f"Verified {needles}"))
    return results


def check_no_md_outside_docs() -> tuple[bool, str]:
    docs_real = os.path.normcase(os.path.realpath(DOCS_DIR))
    violations = []
    for root, dirs, files in os.walk(REPO_ROOT):
        # Skip .git directory
        dirs[:] = [d for d in dirs if d != ".git"]
        for fname in files:
            if fname.lower().endswith(".md"):
                fpath = os.path.realpath(os.path.join(root, fname))
                if not os.path.normcase(fpath).startswith(docs_real + os.sep):
                    violations.append(os.path.relpath(fpath, REPO_ROOT))
    if violations:
        return False, f"Found .md file(s) outside docs/: {violations}"
    return True, "Zero .md files outside docs/"


def main() -> int:
    failures = 0
    checks: list[tuple[str, bool, str]] = []

    ok_crypt, msg_crypt = check_macho_cryptid(OH2_BINARY)
    checks.append(("Mach-O FairPlay cryptid == 0", ok_crypt, msg_crypt))

    checks.extend(check_docs())

    ok_md, msg_md = check_no_md_outside_docs()
    checks.append(("No .md outside docs/", ok_md, msg_md))

    for name, ok, detail in checks:
        tag = "PASS" if ok else "FAIL"
        if not ok:
            failures += 1
        print(f"[{tag}] {name} : {detail}")

    if failures:
        print(f"\nGATE M0: FAIL ({failures} failure(s))")
        return 1

    print("\nGATE M0: PASS (all checks green)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
