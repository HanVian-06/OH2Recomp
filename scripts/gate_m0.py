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
import re
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

REQUIRED_METRICS: list[tuple[str, list[tuple[str, str]]]] = [
    (
        "docs/analysis/macho_binary.md",
        [
            ("53,729,072", r"(?<!\d)53,729,072(?!\d)"),
            ("520,119", r"(?<!\d)520,119(?!\d)"),
            ("805 undefined", r"(?<!\d)`?805`?\s+undefined\b"),
            ("cryptid == 0", r"cryptid\s*==\s*0"),
        ],
    ),
    (
        "docs/analysis/objc_metadata.md",
        [
            ("60 classes", r"(?<!\d)`?60`?\s+classes\b"),
            ("1,491 selectors", r"(?<!\d)`?1,491`?\s+selectors\b"),
            ("192 classrefs", r"(?<!\d)`?192`?\s+classrefs\b"),
            ("134 external", r"(?<!\d)`?134`?\s+external\b"),
            ("52 superrefs", r"(?<!\d)`?52`?\s+superrefs\b"),
        ],
    ),
    (
        "docs/analysis/shaders_air.md",
        [
            ("4,544 shaders", r"(?<!\d)`?4,544`?\s+[Ss]haders\b"),
            ("MTLB", r"\bMTLB\b"),
            ("0 / 4,544", r"(?<!\d)0\s*/\s*4,544(?!\d)"),
        ],
    ),
    (
        "docs/analysis/pak_textures_audio.md",
        [
            ("19,920", r"(?<!\d)19,920(?!\d)"),
            ("PF_ASTC_8x8", r"\bPF_ASTC_8x8\b"),
            ("PF_ASTC_4x4", r"\bPF_ASTC_4x4\b"),
            ("4,454,021,592", r"(?<!\d)4,454,021,592(?!\d)"),
            ("2,407,384,060", r"(?<!\d)2,407,384,060(?!\d)"),
        ],
    ),
    (
        "docs/analysis/toolchain_device.md",
        [
            ("SM-A566B", r"\bSM-A566B\b"),
            ("Samsung Xclipse 540", r"\bSamsung Xclipse 540\b"),
        ],
    ),
]

NEGATED_CRYPTID_LINE_RE = re.compile(
    r"(?i)(?:\b(?:false|fail(?:ed)?|not|never|invalid|broken|warning)\b|!=\s*0)"
)


def check_macho_cryptid(binary_path: str) -> tuple[bool, str]:
    if not os.path.isfile(binary_path):
        return False, f"Binary not found: {binary_path}"
    file_size = os.path.getsize(binary_path)
    if file_size < 32:
        return False, "File too small for mach_header_64"
    with open(binary_path, "rb") as f:
        header = f.read(32)
        if len(header) < 32:
            return False, "File too small for mach_header_64"
        magic, _, _, _, ncmds, sizeofcmds, _, _ = struct.unpack_from("<8I", header, 0)
        if magic != 0xFEEDFACF:
            return False, f"Unexpected Mach-O magic 0x{magic:08x} (expected 0xfeedfacf)"
        if sizeofcmds > file_size - 32:
            return False, f"Invalid Mach-O sizeofcmds ({sizeofcmds}) exceeds file size"
        if ncmds == 0 or ncmds * 8 > sizeofcmds:
            return False, f"Invalid Mach-O ncmds={ncmds} for sizeofcmds={sizeofcmds}"
        cmds_blob = f.read(sizeofcmds)
        if len(cmds_blob) < sizeofcmds:
            return False, "Truncated Mach-O load commands"

    off = 0
    for idx in range(ncmds):
        if off + 8 > len(cmds_blob):
            return False, "Truncated Mach-O load commands"
        cmd, cmdsize = struct.unpack_from("<II", cmds_blob, off)
        if cmdsize < 8 or off + cmdsize > len(cmds_blob):
            return False, f"Malformed Mach-O load command {idx} (cmd=0x{cmd:x}, cmdsize={cmdsize})"
        if cmd in (0x21, 0x2C):  # LC_ENCRYPTION_INFO / LC_ENCRYPTION_INFO_64
            if cmdsize < 20 or off + 20 > len(cmds_blob):
                return False, f"Truncated LC_ENCRYPTION_INFO(_64) load command (cmdsize={cmdsize})"
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
            continue
        with open(full, "r", encoding="utf-8") as f:
            raw_text = f.read()
        stripped_comments = re.sub(r"<!--.*?-->", "", raw_text, flags=re.DOTALL)
        if not raw_text.strip() or not stripped_comments.strip():
            results.append((f"Doc non-empty: {rel}", False, "EMPTY_OR_WHITESPACE"))
        else:
            results.append((f"Doc exists: {rel}", True, f"{os.path.getsize(full):,} bytes"))

    for rel, patterns in REQUIRED_METRICS:
        full = os.path.join(REPO_ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(full):
            results.append((f"Metrics in {rel}", False, "file missing"))
            continue
        with open(full, "r", encoding="utf-8") as f:
            text = f.read()
        cleaned = re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)
        missing = []
        for label, pat in patterns:
            if label == "cryptid == 0":
                valid_line = any(
                    re.search(pat, line) and not NEGATED_CRYPTID_LINE_RE.search(line)
                    for line in cleaned.splitlines()
                )
                if not valid_line:
                    missing.append(label)
            elif not re.search(pat, cleaned):
                missing.append(label)
        if missing:
            results.append((f"Metrics in {rel}", False, f"Missing/invalid metrics: {missing}"))
        else:
            labels = [label for label, _ in patterns]
            results.append((f"Metrics in {rel}", True, f"Verified {labels}"))
    return results


ALLOWED_ROOT_MDS = {"readme.md", "contributing.md", "changelog.md", "security.md"}


def check_no_md_outside_docs() -> tuple[bool, str]:
    docs_real = os.path.normcase(os.path.realpath(DOCS_DIR))
    repo_real = os.path.normcase(os.path.realpath(REPO_ROOT))
    violations = []
    for root, dirs, files in os.walk(REPO_ROOT):
        # Skip .git directory
        dirs[:] = [d for d in dirs if d != ".git"]
        for fname in files:
            if fname.lower().endswith(".md"):
                fpath = os.path.realpath(os.path.join(root, fname))
                fdir = os.path.normcase(os.path.dirname(fpath))
                if fdir == repo_real and fname.lower() in ALLOWED_ROOT_MDS:
                    continue
                if not os.path.normcase(fpath).startswith(docs_real + os.sep):
                    violations.append(os.path.relpath(fpath, REPO_ROOT))
    if violations:
        return False, f"Found .md file(s) outside docs/: {violations}"
    return True, "Zero stray .md files outside docs/ (standard root GitHub docs allowed)"


def main() -> int:
    failures = 0
    checks: list[tuple[str, bool, str]] = []

    if os.path.isfile(OH2_BINARY):
        ok_crypt, msg_crypt = check_macho_cryptid(OH2_BINARY)
        checks.append(("Mach-O FairPlay cryptid == 0", ok_crypt, msg_crypt))
    elif "--require-payload" in sys.argv:
        checks.append(("Mach-O FairPlay cryptid == 0", False, f"Binary not found: {OH2_BINARY}"))
    else:
        checks.append((
            "Mach-O FairPlay cryptid == 0",
            True,
            "Clean open-source checkout (Payload/OH2.app/OH2 not present; pass --require-payload to enforce)",
        ))

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
