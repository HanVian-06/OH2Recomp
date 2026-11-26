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

## ADR-007: Apple OSS (`objc4` / CF / `libdispatch` / Blocks) Evaluation vs Mach-O 64-bit ABI Bridge
- **Date**: 2026-11-22
- **Status**: Accepted
- **Context**: M2 requires full ARM64 Objective-C message dispatch (`objc_msgSend`, `objc_msgSendSuper2`, `objc_msgSend_stret` preserving `x0..x8` and `q0..q7`), ARC/autorelease pools, `@synchronized`, Blocks, GCD, CoreFoundation/Foundation/UIKit bridges, and safe `PLCrashReporter`/GameKit/StoreKit/CloudKit stubs for `Payload/OH2.app/OH2`.
- **Decision**:
  1. Rejected GNUstep `libobjc2` due to incompatible class/`method_list_t`/selector ABI relative to Apple's ARM64 Mach-O runtime layout used across OH2's `60` internal classes and `134` external classrefs.
  2. Evaluated porting upstream Apple OSS `objc4`, `swift-corelibs-foundation`, and `libdispatch` onto Android Bionic: compiling those upstreams cleanly without XNU Mach IPC ports (`mach_msg`, `bootstrap_look_up`, `voucher_t`) introduces heavy build/porting churn for Mach primitives that do not exist on Linux. Instead, extend M1's exact Mach-O 64-bit `DarwinObjCClass` / `DarwinCFString` / `DarwinObjCPrefix` runtime (`src/runtime/`) with dedicated ARM64 assembly trampolines (`objc_msgSend_arm64.S`), real thread-safe `@synchronized`, thread-local autorelease pools, Apple-ABI `_Block_copy`/`_Block_release`, GCD queues/semaphores, toll-free CF <-> Foundation containers, real `NSThread`/`NSTimer`/`CADisplayLink`, and direct `PLCrashReporter` + Mach-port isolation.
- **Consequences**: Zero Mach kernel IPC dependencies, 100% layout fidelity with `OH2`'s Mach-O ObjC metadata, and clean coexistence with `gate_m1.ps1`.

## ADR-008: Darwin `libc_wrapper` ABI Translation & Bundle Path Strategy
- **Date**: 2026-11-22
- **Status**: Accepted
- **Context**: `OH2` invokes Darwin `libSystem.B.dylib` APIs whose struct layouts and constants diverge silently from Android Bionic (`EAGAIN` is `35` on Darwin vs `11` on Linux; BSD `sockaddr_in` carries `uint8_t sin_len` at `+0`; `struct stat` is `144` bytes; `struct dirent` places `d_name` at `+21` after `d_seekoff`; `open()` flags differ; `pthread_mutex_t` uses `0x32aaaba7`/`0x32aaaba2` magic; `FMallocBinned2` assumes `0x4000` 16 KiB page alignment; iOS case-insensitive APFS paths use mixed casing).
- **Decision**: Implement a dedicated Darwin-to-Bionic C translation layer (`src/libc_wrapper/`) registering overrides via `oh2_symbol_register_override` and `oh2_variadic_set_open_handler`: bidirectional errno translation in `___error()`, BSD `sockaddr_in`/`sockaddr_in6`/`addrinfo`/`SOL_SOCKET`/`SO_NOSIGPIPE` translation, 144-byte `DarwinStat64` and per-stream `DarwinDirent64`, multi-component case-insensitive path resolution with `/cookeddata/` fallback mapping `@executable_path/` and `/var/.../OH2.app/` -> `/sdcard/Android/data/com.oceanhorn2.port/files/`, lazy-initialized static mutex/cond/rwlock + 2 MiB minimum worker stack, and `posix_memalign`-backed 16 KiB `oh2_darwin_malloc`.
- **Consequences**: Unmodified `OH2` `MainAppThread:` discovers, opens, stats, and reads `oh2-ios.pak` directly from Android app storage without re-cooking assets.

## ADR-009: Metal Programmable Blending / Framebuffer Fetch (`air.render_target`) Lowering via In-Pass Snapshot Texture & `OpImageFetch`
- **Date**: 2026-11-23
- **Status**: Accepted
- **Context**: Oceanhorn 2 (UE4.23 iOS Mobile HDR) executes an in-pass full-screen tone-compression subpass draw at the end of its 3D Base Pass (`RP#4 D#121`, `Main_0000053e_e1647fe1`, plus translucent/DOF passes) that reads the current pixel of `color(0)` (`inColor [[color(0)]]`, AIR 2.0 metadata `kind == "air.render_target"`). Vulkan 1.1 dynamic rendering (`VK_KHR_dynamic_rendering`) does not support tile subpass `subpassLoad` without `VK_EXT_rasterization_order_attachment_access` / input attachment remapping, and lowering `air.render_target` to a null constant overwrites the entire 3D scene color attachment with `(0, 0, 0, 0)`.
- **Decision**:
  1. In `tools/air2spirv/air_to_spirv.py`, lower any fragment shader input argument with `kind == "air.render_target"` to an `OpImageFetch` (`Lod 0`) at `ivec2/uvec2(gl_FragCoord.xy)` from synthetic Metal 2D texture slot `30` (Vulkan `DescriptorSet 1`, `Binding 62`), and mark the shader entry with `SPIRV_ENTRY_FLAG_FB_FETCH = (1u << 5)`.
  2. In `src/metal_vk/mtl_vk_encoder.cpp` (`snapshot_color0_for_fb_fetch`), before any draw call whose fragment shader has `SPIRV_ENTRY_FLAG_FB_FETCH`, end the current render pass instance, `vkCmdCopyImage` `color[0]` into a cached GPU texture (`st.fb_fetch_image` / `st.fb_fetch_view`), transition it to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`, bind it to fragment texture slot `30`, and resume the render pass with `kMTLLoadActionLoad` on all color/depth/stencil attachments.
- **Consequences**: Preserves 100% of UE4's Mobile HDR tone-compression and programmable-blending shaders under both `VK_KHR_dynamic_rendering` and `VkRenderPass` fallback paths with zero Vulkan validation errors.

## ADR-010: Apple `MTLColorWriteMask` Bit Reversal, Descriptor Value-Copy Semantics, Sticky Immersive UI & AAudio Lifecycle Management
- **Date**: 2026-11-23
- **Status**: Accepted
- **Context**:
  1. Apple's `MTLColorWriteMask` (`Red = 0x8, Green = 0x4, Blue = 0x2, Alpha = 0x1`) reverses bit positions relative to Vulkan's `VkColorComponentFlags` (`R = 0x1, G = 0x2, B = 0x4, A = 0x8`).
  2. UE4's `FMetalStateCache` mutates reusable `MTLRenderPipelineDescriptor`, `MTLDepthStencilDescriptor`, `MTLVertexDescriptor`, and `MTLRenderPassDescriptor` templates and snapshots them via `-[NSObject copy]` / `copyWithZone:` and `setObject:atIndexedSubscript:`.
  3. On Android 17 (One UI / Galaxy A56), `ANativeActivity` leaves the system navigation bar visible unless `WindowInsetsController` / `setSystemUiVisibility(0x1706)` is invoked on the Java UI thread, and `AAudio` continues playing when the user presses Home unless paused on `APP_CMD_PAUSE`/`LOST_FOCUS`/`STOP`.
- **Decision**:
  1. Translate `MTLColorWriteMask` bit-for-bit (`0x8 -> R`, `0x4 -> G`, `0x2 -> B`, `0x1 -> A`) and implement deep-copy `init`, `copy`, `copyWithZone:`, `reset`, and `setObject:atIndexedSubscript:` on all Metal descriptor classes.
  2. Chain `ANativeActivityCallbacks` (`onWindowFocusChanged`, `onResume`, `onNativeWindowCreated`) on the Android main UI thread to enforce sticky immersive fullscreen (`WindowInsetsController.hide(7)` + `BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE` + `setSystemUiVisibility(0x1706)`), and pause/flush the `AAudio` stream whenever the activity loses focus or backgrounds.
- **Consequences**: Eliminates solid black/white boxes around UI elements/fonts, prevents accidental navigation-bar exits during touch gameplay, and stops audio immediately when backgrounding the app.


## D15: Standalone On-Device C++17 AIR 2.0 Compiler & Clean Asset-Free Release APK
- **Decision**: Move AIR 2.0 → SPIR-V 1.3 shader compilation from offline PC-only Python scripts into a native C++17 library (`src/air2spirv_cpp/`) invoked from the Arcadian Android Launcher after the user imports their own decrypted `.ipa`.
- **Rationale**: Keeps the open-source repository and Release APKs 100% free of copyrighted game assets/binaries while achieving ~3-second multi-threaded on-device compilation of all 4,544 shaders.

## D16: Vector `air.convert.f.v*` Support & Metal-to-Vulkan Frame-Pacing / Resource Pooling
- **Decision**: Implement vector float/int conversion intrinsics (`air.convert.f.v*`) in both C++17 and Python AIR translators; replace per-pass CPU buffer-texture hashing with GPU-preamble DMA (`slot.preamble_cmd`); pool `MTLBuffer` allocations (`4 KiB..1 MiB`), `fb_fetch` snapshot images by `(w, h, fmt, occ)`, and per-`(level, slice)` attachment `VkImageView`s; and lock `CADisplayLink` directly to hardware `AChoreographer` VSync timestamps with big-core affinity for UE4 threads.
- **Consequences**: Fixes outdoor CSM shadow cascade corruption while eliminating per-frame `vmaCreateImage`/`vkResetDescriptorPool` thrashing (`drst` 180 → 0..4), CPU buffer-texture readback stalls (~21 ms/frame), and open-loop VSync phase drift.
