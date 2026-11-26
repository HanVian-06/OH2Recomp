# Host Toolchain & Target Device Analysis

**Reproducible Verification Scripts**:
- `powershell -ExecutionPolicy Bypass -File scripts/check_env.ps1`
- `powershell -ExecutionPolicy Bypass -File scripts/analysis/inspect_toolchain_device.ps1`

---

## 1. Host Environment & Build Toolchain (Verified)

| Component | Verified Path / Version | Notes |
|---|---|---|
| **Android SDK** | `%LOCALAPPDATA%\Android\Sdk` | `platform-tools` (`adb` 37.0.1), `build-tools 36.0.0`, `platforms/android-36.1` & `android-37.0` |
| **Android NDK** | `28.2.13676358` (`r28c`) | `clang 19.0.1` (`aarch64-linux-android29-clang++.cmd`), `llvm-readelf`, `llvm-objdump`, `ld.lld` |
| **CMake & Ninja** | CMake `3.22.1-g37088a8-dirty`, Ninja `1.10.2` | `...\Android\Sdk\cmake\3.22.1\bin` (prepended by `scripts/env.ps1`) |
| **Vulkan SDK** | `C:\VulkanSDK\1.4.350.0` | `spirv-val` (`v2026.2`), `glslc` (`shaderc v2026.2`), `spirv-cross` (`1.4.341.0-67`) |
| **JDK & Gradle** | Temurin JDK `25.0.4.1`, cached `gradle-9.3.1` | Use Gradle `9.x` + JDK 25-compatible AGP (ADR-004) |
| **Python 3** | `Python 3.14.7` (`pip 26.2.2`) | Installed `macholib 1.16.4`, `capstone 5.0.9`, `lief 1.0.0` (`cp312-abi3` wheel), `pyelftools 0.33`, `numpy 2.5.4`, `lz4 4.4.5` |
| **Git** | `git version 2.55.0.windows.5` | Repo initialized at `g:\Projec Porting`; local `user.name = Porting Bot`, `Payload/` in `.gitignore` |

---

## 2. Vendored Vulkan Validation Layer (`arm64-v8a`)
- **Source**: Official Khronos `Vulkan-ValidationLayers` release `vulkan-sdk-1.4.350.0` (`android-binaries-1.4.350.0.zip`)
- **Path**: `third_party/vulkan_validation/arm64-v8a/libVkLayer_khronos_validation.so`
- **Size**: `26,345,704` bytes (`25.13 MiB`)
- **SHA256**: `34A741D51CB6E9111EC52CDA20EEE812BCFBCD197348C1404232AACB60E89EF3`
- **ELF Verification (`llvm-readelf -h`)**: `Class: ELF64`, `Type: DYN (Shared object file)`, `Machine: AArch64`

---

## 3. Target Device (`adb` — Samsung Galaxy A56)

| Property | Verified Value |
|---|---|
| **ADB Serial / State** | `RRCY40086KE` (`device`, `product:a56xnaxx`, `model:SM_A566B`) |
| **Model / Device / SoC** | `SM-A566B` / `a56x` / `s5e8855` (Samsung Exynos 1580, `erd8855`) |
| **OS / ABI** | Android `17` (`ro.build.version.sdk = 37`), `arm64-v8a` |
| **RAM (`/proc/meminfo`)** | `MemTotal: 7,594,824 kB` (~`7.24 GiB`, meets `>= 6 GB` requirement) |
| **GPU (`cmd gpu vkjson`)** | `Samsung Xclipse 540` (`vendorID: 0x144d`, `deviceID: 0x02300100`) |
| **Vulkan Version** | Device `apiVersion = 4206896` (`1.3.304`), `android.hardware.vulkan.level = 1` |
| **ASTC LDR Support** | `textureCompressionASTC_LDR = 1.0` (**Supported**) |
| **Key Extensions** | `VK_KHR_dynamic_rendering = True`, `VK_EXT_descriptor_indexing = True` |

*(Non-Galaxy-A56 chipsets such as Adreno or Mali/Immortalis are `"belum teruji"` until physically verified.)*
