# Contributing

Thank you for your interest in contributing to **OH2Recomp**!

## Legal & Repository Hygiene

- **Never commit copyrighted game files** (`.ipa` files, `Payload/OH2.app/`, `.pak` archives, `.ushaderbytecode` / `.mtlb` binaries, extracted icons/textures/audio, or generated `spirv_cache.bin`).
- **Never commit build artifacts, APKs, tokens, or personal keystores** (`build/`, `*.apk`, `*.keystore`, `*.jks`, `scripts/env.local.ps1`).
- Keep any local `Payload/OH2.app/` directory strictly read-only.
- Place any new detailed technical documentation inside `docs/` (only standard GitHub root files `README.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, `SECURITY.md` are permitted outside `docs/`).

## Verification Before Opening a Pull Request

1. Run the M0 documentation & structure hygiene gate:
   ```powershell
   python scripts/gate_m0.py
   ```
2. Verify the clean open-source Release APK (`arm64-v8a`) compiles cleanly without requiring copyrighted assets:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/gate_m5.ps1 -Release -BuildOnly
   ```
3. If you modified `src/air2spirv_cpp/`, `tools/air2spirv/`, or `src/metal_vk/spirv_cache.*` and have local `Payload/OH2.app/` available, run the M4 shader validation gate (must maintain 100% `spirv-val --target-env vulkan1.1` pass across all 4,544 shaders):
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/gate_m4.ps1 -Workers 8
   ```
