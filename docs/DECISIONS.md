# Architecture Decision Records (ADRs)

## ADR-001: Runtime Mach-O Loader (`LC_DYLD_INFO_ONLY`) vs Offline Mach-O -> ELF Conversion
- **Date**: 2026-11-22
- **Status**: Accepted
- **Context**: `Payload/OH2.app/OH2` is a single thin ARM64 `MH_EXECUTE` Mach-O (`53,729,072` bytes, `cryptid == 0`) using `LC_DYLD_INFO_ONLY` (`520,119` rebase pointers, `4,040` bind sites, `672` weak_bind sites, `539` lazy_bind sites, `466` `__mod_init_func`, `805` undefined symbols across `27` active dylibs). Offline Mach-O -> ELF rewriting risks brittle relocation/C++ exception/ObjC section re-layout and violates Human Checkpoint 3 boundaries if abandoned mid-flight.
- **Decision**: Implement a **Runtime Mach-O Loader** (`src/loader/`) that `mmap`s `__TEXT` (RX) and `__DATA` (RW) directly, processes `LC_DYLD_INFO_ONLY` fixups, runs `__mod_init_func`, bridges `__unwind_info`/`__eh_frame`, and routes Darwin imports (`std::__1`, variadic stack args, `libSystem`, `libobjc`, frameworks) to Android shims via `darwin_resolve_symbol`.
- **Consequences**: Preserves original virtual-address relative layout and ObjC/C++ metadata pointers verbatim; requires Darwin-to-AAPCS64 variadic/`va_list` assembly trampolines (`12 + 2` symbols) and `std::__1` ABI symbol mapping (`16` symbols).

## ADR-002: Offline AIR 2.0 LLVM Bitcode -> SPIR-V Shader Translation
- **Date**: 2026-11-22
- **Status**: Accepted
- **Context**: Full inspection of `oh2-ios.pak` via `scripts/analysis/inspect_shaders_pak.py` proved that **0 / 4,544** shaders contain MSL source code. **4,544 / 4,544** shaders (`317` Global + `4,227` OH2) are zlib-compressed `FMetalCodeHeader` + `MTLB` containers wrapping Apple Bitcode (`0x0b17c0de`) + LLVM Bitcode (`BC\xc0\xde`, AIR 2.0 / Metal 2.0). `SPIRV-Cross` parses SPIR-V, not AIR bitcode.
- **Decision**: Build an offline **AIR 2.0 LLVM Bitcode -> SPIR-V** translator (`tools/air2spirv/`) targeting `spirv_cache.bin`, validated offline by `spirv-val` (gate `>= 90%` pass rate). Notably, `0 / 4,544` shaders use tessellation, function constants, argument buffers, raytracing, or tile/imageblock shaders, bounding translator scope to VS/PS/CS + `air.*` intrinsics.
- **Consequences**: Zero runtime shader compilation overhead on device; shader translation defects are caught offline via `spirv-val` before M5 integration.

## ADR-003: Vulkan 1.1 Core Baseline with Runtime Feature Detection
- **Date**: 2026-11-22
- **Status**: Accepted
- **Context**: Target Android devices span Adreno, Mali/Immortalis, and Samsung Xclipse GPUs (`arm64-v8a`, `>= 6 GB` RAM). While Galaxy A56 (`Samsung Xclipse 540`, Vulkan `1.3.304`) supports `VK_KHR_dynamic_rendering`, `VK_EXT_descriptor_indexing`, and `textureCompressionASTC_LDR`, older or mid-tier Vulkan 1.1 drivers may not expose Vulkan 1.3 core promotions.
- **Decision**: Target **Vulkan 1.1 core** as the baseline API (`android-29+`). Query `VK_KHR_dynamic_rendering` and `VK_EXT_descriptor_indexing` dynamically at runtime with a mandatory `VkRenderPass` + subpass fallback path. Flip Y using Vulkan 1.1 negative viewport height (with matching front-face winding adjustment) and ingest `PF_ASTC_4x4`/`6x6`/`8x8` natively as `VK_FORMAT_ASTC_*` with a 64–128 MB staging ring-buffer.
- **Consequences**: Guarantees broad Android GPU compatibility without sacrificing modern fast paths on Xclipse 540.

## ADR-004: Gradle 9.3.1 + JDK 25 Compatibility
- **Date**: 2026-11-22
- **Status**: Accepted
- **Context**: Host `JAVA_HOME` is Temurin JDK `25.0.4.1`, and `gradle-9.3.1` is already cached locally (`~/.gradle/wrapper/dists/`). Older Gradle (`< 9.x`) and older AGP versions fail to execute on JDK 25 bytecode/classfile version 69.
- **Decision**: Standardize all Android builds (`android/`) on Gradle `9.x` (`9.3.1`) with a JDK 25-compatible Android Gradle Plugin (`8.10+` / `9.x`), NDK `28.2.13676358` (r28c), and SDK CMake `3.22.1`.
- **Consequences**: Uses existing host JDK/Gradle cache with zero extra JDK installation required.

## ADR-005: Read-Only `Payload/OH2.app/` & Git Exclusion
- **Date**: 2026-11-22
- **Status**: Accepted
- **Context**: `Payload/OH2.app/` (`41` files, `2,491,086,213` bytes / `2.32 GB`) contains user-owned original IPA assets (`oh2-ios.pak` = `2,430,963,623` bytes, `OH2` = `53,729,072` bytes).
- **Decision**: Treat `Payload/` as strictly read-only and exclude `Payload/` in `.gitignore`. Any derived/cooked outputs (e.g., `spirv_cache.bin`) are generated into separate build/asset directories.
- **Consequences**: Keeps Git repository lightweight and protects original source artifacts from accidental mutation.

## ADR-006: Python 3.14 Mach-O Tooling & Vendored Khronos Validation Layer
- **Date**: 2026-11-22
- **Status**: Accepted
- **Context**: Host Python is `3.14.7`. In addition, Android NDK r28c and Windows Vulkan SDK `1.4.350.0` do not bundle Android `arm64-v8a` `libVkLayer_khronos_validation.so` required for M3 validation.
- **Decision**:
  1. Installed `macholib 1.16.4`, `capstone 5.0.9`, and `lief 1.0.0` (`cp312-abi3-win_amd64` wheel loads cleanly on Python `3.14.7`), paired with stdlib `struct`, `numpy`, and NDK `llvm-readelf`/`llvm-objdump`.
  2. Downloaded official Khronos `Vulkan-ValidationLayers` release `vulkan-sdk-1.4.350.0` (`android-binaries-1.4.350.0.zip`) matching the host Vulkan SDK, extracted `arm64-v8a/libVkLayer_khronos_validation.so` (`26,345,704` bytes, stripped `ELF64` `AArch64` `DYN`), and vendored it at `third_party/vulkan_validation/arm64-v8a/libVkLayer_khronos_validation.so` (SHA256 `34A741D51CB6E9111EC52CDA20EEE812BCFBCD197348C1404232AACB60E89EF3`).
- **Consequences**: Full offline Mach-O/ELF inspection and M3 Android Vulkan validation readiness without relying on external runtime downloads during later milestones.
