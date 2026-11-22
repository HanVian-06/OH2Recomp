# Oceanhorn 2 (iOS -> Android) Porting Plan

## 1. Objective & Target Platform
Port iOS **Oceanhorn 2** (`com.cornfox.oceanhorn2`, Unreal Engine `4.23`, single thin ARM64 Mach-O binary `OH2` + `oh2-ios.pak`) to **Android `arm64-v8a`** via a runtime Mach-O loader, Objective-C runtime bridge, Darwin/OS shim, offline AIR-to-SPIR-V shader pipeline, and Metal-to-Vulkan renderer.

- **Primary Test Device**: Samsung Galaxy A56 (`SM-A566B`, `s5e8855` Exynos 1580, Samsung Xclipse 540 Vulkan 1.3.304, Android 17 / SDK 37, ~7.24 GiB RAM).
- **Compatibility Target**: `arm64-v8a`, `>= 6 GB` RAM, all GPU vendors (Adreno, Mali/Immortalis, Xclipse). Untested chipsets must be reported explicitly as `"belum teruji"`.
- **Memory Target**: Peak RSS/PSS `<= 2.5 GB` (`dumpsys meminfo`), no CPU-side RGBA8 decompression of ASTC textures.

---

## 2. Milestones, Dependencies & Repeatable Gates

| Milestone | Scope | Dependencies | Gate Script & Acceptance Criteria | Status |
|---|---|---|---|---|
| **M-1** | Toolchain & device setup (`env.ps1`, `check_env.ps1`, NDK r28c, CMake, Ninja, Vulkan SDK `spirv-val`/`glslc`/`spirv-cross`, JDK 25, Python Mach-O libs, Khronos validation layer SO, Git hygiene, Galaxy A56 `adb`) | None | `scripts/gate_m_minus_1.ps1` exits `0` (all toolchain + `SM-A566B` checks pass) | **DONE** |
| **M0** | Reproducible binary/ObjC/shader/pak/device analysis scripts (`scripts/analysis/*`), `docs/` + `docs/analysis/*`, FairPlay verification | M-1 | `scripts/gate_m0.py` exits `0`: `LC_ENCRYPTION_INFO_64.cryptid == 0`, all `docs/` present with verified metrics, zero `.md` files outside `docs/` | **DONE** |
| **M1** | Headless Mach-O ARM64 loader (`src/loader/`, `android/m1_headless/`): `mmap` `__TEXT`/`__DATA`/`__LINKEDIT`, `LC_DYLD_INFO_ONLY` (`520,119` rebase, `4,040` bind with `219` `+16` RTTI addends, `672` weak_bind, `539` eager lazy_bind), `466` `__mod_init_func`, `__unwind_info`/`__eh_frame` + `___gxx_personality_v0`, `std::__1` (`16` imports), `12` variadic + `2` `va_list` Darwin-to-AAPCS64 trampolines, `pthread`/TLS safety | M0 | `scripts/gate_m1.ps1`: `OH2` loads on Galaxy A56, `0` missing symbols (`805/805`), `466/466` initializers & `LC_MAIN` pass, first UE4 engine log lines observed in `logcat` (`GATE M1: PASS`, exit `0`) | **DONE** |
| **M2** | Runtime & OS Shim (`src/runtime/`, `src/libc_wrapper/`): Apple OSS evaluation (`objc4`/CF/`libdispatch`/Blocks), `objc_msgSend`/`objc_msgSendSuper2` ARM64 assembly (`x0-x7`, `d0-d7`), `libc_wrapper` (`errno`, `sockaddr_in.sin_len`, `stat`, `open`, `pthread`), `NSBundle`/`NSFileManager` (`/sdcard/Android/data/<pkg>/files/`), `UIApplication`/`UIScreen`/`CADisplayLink` lifecycle, `GameKit`/`StoreKit`/`CloudKit`/`MultipeerConnectivity`/`UserNotifications`/`DeviceCheck`/`PLCrashReporter` stubs | M0, M1 | `gate_m2`: UE4 opens `oh2-ios.pak` and verifies the `19,920`-entry asset index | Pending |
| **M3** | Standalone Vulkan 1.1 Graphics Foundation (`src/vk_foundation/`, `android/m3_vk_sandbox/`): `VkInstance`/`VkDevice`/swapchain on `ANativeWindow`, VMA, runtime check for `VK_KHR_dynamic_rendering`/`VK_EXT_descriptor_indexing` with `VkRenderPass` fallback, clear color + triangle, negative viewport Y-flip + winding/cull adjust, native ASTC `VkImage`, 64–128 MB staging ring-buffer, Khronos validation layer enabled | M0 | `scripts/gate_m3.ps1`: Clear color + ASTC triangle rendered on Galaxy A56 via `adb` with `0` Vulkan validation errors/warnings (`GATE M3: PASS`, exit `0`) | **DONE** |
| **M4** | Offline Shader Pipeline (`tools/air2spirv/`, `src/metal_vk/spirv_cache.{h,cpp}`): Extract all `4,544` `MTLB` AIR 2.0 LLVM Bitcode (`0x0b17c0de` -> `BC\xc0\xde`) shaders (`4,482` unique `MTLB` containers) from `.ushaderbytecode` in `oh2-ios.pak`, translate AIR 2.0 -> Vulkan 1.1 SPIR-V 1.3 (`0x00010300`), pack `SPVC` v1 `build/shaders/spirv_cache.bin`, validate every SPIR-V module with `spirv-val` (`--target-env vulkan1.1`) | M0 | `scripts/gate_m4.ps1`: `spirv_cache.bin` (`69,057,232` B) generated and `4,544 / 4,544` (`100.00%` `>= 90%`) shaders pass `spirv-val --target-env vulkan1.1` (`GATE M4: PASS`, exit `0`) | **DONE** |
| **M5** | Metal-to-Vulkan Shim & Full Integration (`src/metal_vk/`, `src/platform/`): `MTLDevice`, `MTLCommandQueue`, `MTLCommandBuffer`, `MTLRenderCommandEncoder`, `MTLComputeCommandEncoder`, `CAMetalLayer`, render pass mapping, `spirv_cache.bin` PSOs, `AInputQueue` -> `UITouch`, `AChoreographer` -> `CADisplayLink`, `IOSAudio`/`ADPCM` -> `AAudio` | M1, M2, M3, M4 | `gate_m5`: Main menu rendered and touch-responsive with audio on Samsung Galaxy A56 via `adb` | Pending |
| **M6** | Memory Budget & Chipset Compatibility (`src/memory/`, `src/compat/`): `onTrimMemory`, `.pak` `mmap` + bounded compressed chunk streaming (`r.Streaming.PoolSize <= 256-384 MB`), zero RGBA8 ASTC decompression, chipset compatibility probe (`"teruji"` vs `"belum teruji"`) | M5 | `gate_m6`: Peak `dumpsys meminfo` <= `2.5 GB`, zero RGBA8 ASTC expansion, untested chipsets labeled `"belum teruji"` | Pending |
| **E2E** | Opaque-box 4-tier E2E testing + Tier-5 adversarial hardening (`tests/`) | M0..M6 | All E2E tiers pass without regressions | Pending |

---

## 3. Iteration Loop & Escalation Rules
- **Standard Iteration Loop**: `build -> install -> run -> logcat/tombstone -> classify (missing symbol, SIGSEGV, validation error, OOM) -> fix -> repeat`.
- **3-Failure Rule**: If the same gate fails 3 consecutive times, record the root cause in `docs/BLOCKERS.md`, mark the item blocked, and advance independent milestones.
- **Human Checkpoints (ONLY reasons to pause for user input, always with options + recommendation)**:
  1. **`cryptid != 0` (FairPlay DRM)**: Hard stop; request a decrypted binary dump. *(M0 status: `cryptid == 0` -> CLEAR.)*
  2. **Shader Infeasibility / `< 90%` Pass Rate**: If AIR 2.0 -> SPIR-V translation cannot achieve `>= 90%` `spirv-val` pass rate in M4.
  3. **Costly / Hard-to-Reverse Architectural Pivot**: E.g., switching from Runtime Mach-O Loader to offline Mach-O -> ELF static conversion after M1 is underway.
