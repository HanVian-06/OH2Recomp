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


## 2026-11-22 — M0 Iteration 2 Hardening & Documentation Precision
- **Script Hardening**:
  - `scripts/check_env.ps1`: Parsed matched Galaxy A56 serial from `adb devices -l` and passed `-s $Serial` to `adb shell getprop` so concurrent attached devices/emulators cannot trigger `more than one device/emulator`.
  - `scripts/env.ps1`: Trimmed whitespace segments when deduplicating `PATH` entries so space-padded `PATH` entries do not duplicate `platform-tools` on first dot-source.
  - `scripts/gate_m_minus_1.ps1`: Added non-zero file length and `\x7fELF` 4-byte magic verification for `third_party/vulkan_validation/arm64-v8a/libVkLayer_khronos_validation.so`.
  - `scripts/gate_m0.py`: Added Mach-O `ncmds`/`sizeofcmds`/`cmdsize >= 8`/`cmdsize >= 20` bounds guards to prevent `cmdsize == 0` hangs or `struct.error`, plus whitespace/comment stripping and boundary-aware non-negated metric regex validation in `check_docs()`.
  - `scripts/analysis/inspect_macho.py`: Removed redundant `read_uleb128` call in `count_export_trie`.
- **Documentation Precision**:
  - `docs/analysis/pak_textures_audio.md`: Updated `.pak` uncompressed payload total (`4,454,021,592` B / `4,247.69 MB`), compressed payload total (`2,407,384,060` B), and `.png/.res/.psd/.locres/.ini/other` row (`2,247,305` B compressed, `5,016,980` B uncompressed) to match `inspect_shaders_pak.py --json`.
  - `docs/analysis/macho_binary.md`: Updated UE4 build tag string on line 5 to `++UE4+Release-4.23 May 22 2020 +`.


## 2026-11-22 — M3 Standalone Vulkan 1.1 Graphics Foundation (`M3_GRAPHICS_FOUNDATION`)
- **Vulkan 1.1 Core & VMA Foundation (`src/vk_foundation/`, `third_party/vma/`)**:
  - Integrated Vulkan Memory Allocator `v3.5.0` (`third_party/vma/vk_mem_alloc.h`) with Vulkan 1.1 dynamic function pointers (`VMA_VULKAN_VERSION=1001000`).
  - Implemented `oh2::vk::VkContext` (`src/vk_foundation/src/vk_context.cpp`): Vulkan 1.1 instance with `VK_LAYER_KHRONOS_validation` and `VK_EXT_debug_utils`, physical device and queue selection on `ANativeWindow`, runtime feature detection via `vkGetPhysicalDeviceFeatures2` (`VK_KHR_dynamic_rendering`, `VK_EXT_descriptor_indexing`, and native ASTC 4x4/6x6/8x8 LDR format properties), and sRGB-aware swapchain creation.
  - Implemented `oh2::vk::StagingRingBuffer` (`src/vk_foundation/src/staging_ring.cpp`): persistent mapped 64 MiB VMA staging ring-buffer with contiguous tail wrap-around, `nonCoherentAtomSize` flush alignment, and `VkFence`-tracked span reclamation (verified via 128 MiB GPU transfer self-test with 2 wrap-arounds and 9 fence reclaims).
  - Implemented `oh2::vk::AstcTexture` (`src/vk_foundation/src/astc_texture.cpp`): direct staging-ring-to-`VkImage` upload (`VK_FORMAT_ASTC_4x4_UNORM_BLOCK`) with zero CPU-side RGBA8 decompression.
  - Implemented `oh2::vk::M3Renderer` (`src/vk_foundation/src/m3_renderer.cpp`) and Vulkan 1.1 shaders (`src/vk_foundation/shaders/triangle.{vert,frag}`): dual render paths (`VkRenderPass` fallback + `VK_KHR_dynamic_rendering` both verified at runtime), negative-viewport Y-flip (`y=H, height=-H`) with CCW front-face winding and back-face culling, and 7-stage automated self-test logging.
- **Android Sandbox & Repeatable Gate (`android/m3_vk_sandbox/`, `scripts/m3/`, `scripts/gate_m3.ps1`)**:
  - Created `android/m3_vk_sandbox` (`com.oceanhorn2.port.m3vulkan`), offline APK builder `scripts/m3/build_apk.ps1` (`glslc` + `spirv-val` + CMake/Ninja NDK r28c + `aapt2` + `ZIP_STORED` native libs + `zipalign` + `apksigner`), pixel verifier `scripts/m3/verify_screencap.py`, and gate `scripts/gate_m3.ps1`.
  - Verified on Samsung Galaxy A56 (`RRCY40086KE`, `Samsung Xclipse 540`): all 7 `[M3_PASS]` checks passed, `validation_errors=0 validation_warnings=0`, screenshot `2340x1080` verified (`clear_ratio=0.784`, `tri_ratio=0.216`, upright Y-flip `lower/upper=3.03x`), `scripts/gate_m3.ps1` exited `0` (`GATE M3: PASS`).
  - Documented architecture and device verification results in `docs/analysis/m3_vulkan.md`.


## 2026-11-22 — M1 Headless Mach-O ARM64 Runtime Loader (`M1_LOADER`)
- **Mach-O 64-bit Loader & Dyld Opcode Engine (`src/loader/macho_loader.{h,cpp}`, `src/loader/dyld_fixups.cpp`)**:
  - Enforced `LC_ENCRYPTION_INFO_64.cryptid == 0` at load time, reserved contiguous `53.23 MiB` VA range at preferred base `0x100000000 .. 0x10353c000` (`slide = 0x0`), mapped `__TEXT`, `__DATA` (with `0x1fc000` bytes of `__bss`/`__common` tail zero-filled), and `__LINKEDIT`, and verified `0` `MRS Xn, TPIDRRO_EL0` reads across all `10,503,788` `__TEXT,__text` instructions.
  - Applied all `LC_DYLD_INFO_ONLY` fixups: `520,119` rebase pointers, `4,040` bind writes (including `219` `+16` RTTI vtable addends), `539` eager `lazy_bind` slots, and `672` `weak_bind` overrides with `0` missing symbols.
- **805-Symbol Resolver, Variadic Trampolines & Unwind Bridge (`src/loader/symbol_resolver.cpp`, `src/loader/variadic_trampolines.S`, `src/loader/variadic_wrappers.c`, `src/loader/unwind_bridge.cpp`)**:
  - Resolved all `805/805` undefined symbols (`947` total symbol table entries including `140` external ObjC classes + `140` metaclasses, `14` Group-A + `98` Group-B data globals, `16` `std::__1`/`__cxxabiv1` symbols, `12` variadic + `2` `va_list` Darwin-to-AAPCS64 trampolines, and `60` internal ObjC classes).
  - Registered all `19` `__TEXT,__eh_frame` FDEs and `3,951` synthesized DWARF FDEs (`838` LSDAs) from `__TEXT,__unwind_info` into `-static-libstdc++` `libunwind`.
  - Fixed two Darwin-vs-Linux ABI bugs exposed during `FMallocBinned2` startup (`mod_init_idx=12`): clamped `_host_statistics` to `HOST_VM_INFO_COUNT = 15` words (`60` B) after discovering `FIOSPlatformMemory::GetConstants` passes `sizeof(vm_statistics_data_t) = 60` as the word count, and translated Darwin `MAP_ANON (0x1000)` to Linux `MAP_ANONYMOUS (0x20)` with 64 KiB-aligned anonymous VM mappings in `darwin_mmap` / `darwin_vm_allocate`.
- **On-Device Verification (`android/m1_headless/`, `scripts/gate_m1.ps1`, `docs/analysis/m1_loader.md`)**:
  - Verified on Samsung Galaxy A56 (`RRCY40086KE`, `SM-A566B`): all `466/466` `__mod_init_func` initializers and `LC_MAIN` (`0x10058e3bc` -> `UIApplicationMain` -> `-[IOSAppDelegate application:didFinishLaunchingWithOptions:]` at `0x100dbd5d0`) executed cleanly (`EXIT_CODE=0`), emitting UE4 engine logs (`[UE4] Used memory before allocating anything was 0.00MB`, `========= This app is in LANDSCAPE mode`, `[UE4] LogInit: Oceanhorn 2 UE4.23 Mach-O M1 Runtime Active on Android ARM64`), `scripts/gate_m1.ps1` exited `0` (`GATE M1: PASS`).


## 2026-11-22 — M4 Offline AIR 2.0 LLVM Bitcode -> SPIR-V 1.3 Shader Pipeline (`M4_SHADER_PIPELINE`)
- **AIR 2.0 LLVM Bitcode Decoder & SPIR-V 1.3 Translator (`tools/air2spirv/`)**:
  - Implemented `tools/air2spirv/bitcode_reader.py`: pure-Python LLVM bitstream (`0x0b17c0de` / `BC\xc0\xde`) and Apple `MTLB` / `FMetalCodeHeader` parser decoding all `15` type codes, `7` constant codes, `5` metadata codes (`METADATA_STRINGS=35`, `!air.vertex`, `!air.fragment`, `!air.kernel`), `18` SSA instruction opcodes (`1,487,206` main instructions), and target datalayout byte offsets across all `4,482` unique `MTLB` modules (`4,544` total archive shaders: `2,998` PS, `1,537` VS, `9` CS).
  - Implemented `tools/air2spirv/air_to_spirv.py`: deterministic offline translator targeting Vulkan 1.1 SPIR-V 1.3 (`0x00010300`, `OpMemoryModel Logical GLSL450`) with `OpTypeFloat 32` + `RelaxedPrecision` for `half`, 32-bit `OpTypeInt 32 0` integer width normalization, `OpKill` for `air.discard_fragment`, logical pointer byte-offset tracking (`addrspace 0..3`), `reg2mem` cross-block SSA promotion, SESE topological DAG / single-header `OpLoopMerge` structured control flow, and lowering for all `126` `air.*` intrinsics + `7` internal `___metal_fract*` helpers.
  - Implemented `tools/air2spirv/validator.py` and `tools/air2spirv/cli.py`: parallel translation, Vulkan 1.1 `SPIRV-Tools v2026.2` / `spirv-val.exe --target-env vulkan1.1` validation, `SPVC` v1 binary cache packing (`build/shaders/spirv_cache.bin`, `69,057,232` B, `4,482` sorted `112`-byte disk entries, `52,989` descriptor binding records, `7,588` vertex attribute records), and summary report `build/shaders/m4_report.json`.
- **Runtime Loader Contract & Verification Gate (`src/metal_vk/spirv_cache.{h,cpp}`, `scripts/gate_m4.ps1`, `docs/analysis/m4_shaders.md`)**:
  - Implemented `spirv_cache_load`, `spirv_cache_find`, `spirv_cache_hash_mtlb`, `spirv_cache_find_by_mtlb`, `spirv_cache_find_by_pak_sha1`, `spirv_cache_find_by_entry_name`, and `spirv_cache_unload` in `src/metal_vk/spirv_cache.{h,cpp}` (verified with NDK r28c `aarch64-linux-android29-clang++ -Wall -Wextra -Werror`).
  - Verified `4,544 / 4,544` (`100.00%`, `4,482 / 4,482` unique `MTLB` modules: `2,998/2,998` PS, `1,537/1,537` VS, `9/9` CS) pass `spirv-val --target-env vulkan1.1` (`>= 90.0%` threshold), `scripts/gate_m4.ps1` exited `0` (`GATE M4: PASS`).
