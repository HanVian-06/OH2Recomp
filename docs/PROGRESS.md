# Progress Log (Append-Only)

## 2026-11-22 — M-1 Toolchain Setup & M0 Binary/Asset Analysis
- **Git & Hygiene**:
  - Initialized Git repository in `g:\Projec Porting` (`user.name = "Porting Bot"`, `user.email = "porting@local"`).
  - Created `.gitignore` excluding `Payload/`, `build/`, `.gradle/`, `*.apk`, `*.aab`, `__pycache__/`, `.idea/`, `*.tmp`, `*.pyc`, and temporary archives.
  - Commit `08707c5`: `chore(m-1): init repo, .gitignore, and env/check_env scripts`.
- **M-1 Toolchain & Dependencies**:
  - Created `scripts/env.ps1` (dot-sourceable, idempotent `PATH` + `ANDROID_HOME`/`ANDROID_SDK_ROOT`/`ANDROID_NDK_HOME`/`ANDROID_NDK_ROOT`) and `scripts/check_env.ps1` (12 checks, all passing).
  - Installed Python Mach-O inspection packages on Python `3.14.7`: `macholib 1.16.4`, `capstone 5.0.9`, `lief 1.0.0` (`cp312-abi3`).
  - Acquired Khronos `arm64-v8a/libVkLayer_khronos_validation.so` (`vulkan-sdk-1.4.350.0`, `26,345,704` B, verified `ELF64` `AArch64` `DYN` via `llvm-readelf -h`) into `third_party/vulkan_validation/arm64-v8a/`.
- **M0 Reproducible Verification & Documentation**:
  - Created and executed `scripts/analysis/inspect_macho.py`, `scripts/analysis/inspect_objc.py`, `scripts/analysis/inspect_shaders_pak.py`, and `scripts/analysis/inspect_toolchain_device.ps1`.
  - Verified `LC_ENCRYPTION_INFO_64.cryptid == 0` on `Payload/OH2.app/OH2` (`53,729,072` B), `520,119` rebase pointers, `805` undefined symbols, `60` ObjC classes, `1,491` selectors, `4,544` `MTLB` AIR 2.0 bitcode shaders (`0` MSL source), `19,920` `.pak` v8 entries (`bEncryptedIndex=0`), and Galaxy A56 (`SM-A566B`, `Samsung Xclipse 540`, Vulkan `1.3.304`).
  - Created `docs/PLAN.md`, `docs/DECISIONS.md`, `docs/PROGRESS.md`, `docs/BLOCKERS.md`, and `docs/analysis/{macho_binary,objc_metadata,shaders_air,pak_textures_audio,toolchain_device}.md`.
  - Created and passed repeatable gates `scripts/gate_m_minus_1.ps1` (`GATE M-1: PASS`, exit `0`) and `scripts/gate_m0.py` (`GATE M0: PASS`, exit `0`).
