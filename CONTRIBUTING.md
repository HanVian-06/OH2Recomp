# Contributing

Thank you for your interest in contributing to this project!

## Legal & Repository Hygiene

- **Never commit copyrighted game files** (IPAs, `Payload/OH2.app/`, `.pak` archives, extracted textures/audio, or prebuilt `spirv_cache.bin`).
- **Never commit build artifacts, APKs, or personal keystores** (`build/`, `*.apk`, `*.keystore`, `*.jks`).
- Keep `Payload/OH2.app/` strictly read-only.

## Verification Before Opening a Pull Request

1. Run the M0 documentation & structure gate:
   ```powershell
   python scripts/gate_m0.py
   ```
2. If you modified `tools/air2spirv/` or `src/metal_vk/spirv_cache.*`, run the M4 shader validation gate (must remain 100% `spirv-val --target-env vulkan1.1` pass):
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/gate_m4.ps1 -Workers 8
   ```
3. Verify the Android arm64-v8a build compiles cleanly:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/gate_m5.ps1 -BuildOnly
   ```
