# M3 Standalone Vulkan 1.1 Graphics Foundation (`M3_GRAPHICS_FOUNDATION`)

## 1. Architecture Overview
The M3 Graphics Foundation provides a standalone, zero-UE4-dependency **Vulkan 1.1 core** rendering subsystem (`src/vk_foundation/`) and an Android NativeActivity test harness (`android/m3_vk_sandbox/`, package `com.oceanhorn2.port.m3vulkan`) designed to back the M5 Metal-to-Vulkan translation layer.

| Component | Path | Responsibility |
|---|---|---|
| **Core Types & Capabilities** | `src/vk_foundation/include/vk_foundation/vk_types.h` | `VkCapabilities`, `RenderPath` (`RenderPassFallback`, `DynamicRendering`), `Vertex` layout |
| **Vulkan Context & WSI** | `src/vk_foundation/src/vk_context.cpp` | Vulkan 1.1 `VkInstance`, `VK_LAYER_KHRONOS_validation`, `VK_EXT_debug_utils`, `ANativeWindow` surface/swapchain, VMA v3.5.0 allocator, runtime extension/format probing |
| **VMA Staging Ring-Buffer** | `src/vk_foundation/src/staging_ring.cpp` | Persistent mapped 64–128 MiB `VK_BUFFER_USAGE_TRANSFER_SRC_BIT` circular buffer with contiguous wrap-around, `nonCoherentAtomSize` flush alignment, and `VkFence`-driven span reclamation |
| **Native ASTC Texture Upload** | `src/vk_foundation/src/astc_texture.cpp` | Direct GPU upload of compressed ASTC blocks (`VK_FORMAT_ASTC_4x4_UNORM_BLOCK`, `6x6`, `8x8`) via `StagingRingBuffer` and `vkCmdCopyBufferToImage` with zero CPU RGBA8 decompression |
| **Dual-Path Renderer & Self-Test** | `src/vk_foundation/src/m3_renderer.cpp` | Dual pipelines (`VkRenderPass` + `VK_KHR_dynamic_rendering`), Vulkan 1.1 negative-viewport Y-flip, 7-stage automated self-test suite |
| **Android Sandbox Activity** | `android/m3_vk_sandbox/` | Landscape fullscreen `NativeActivity` packaging `libm3_vk_sandbox.so`, `libVkLayer_khronos_validation.so`, and validated SPIR-V shaders |

---

## 2. Runtime Capabilities Verified on Samsung Galaxy A56 (`SM-A566B`)
Measured at runtime by `VkContext::pick_physical_device()` on device `RRCY40086KE`:
- **GPU**: `Samsung Xclipse 540` (`vendorID = 0x144d`, `deviceID = 0x02300100`, driver Vulkan API `1.3.304` / `0x403130`, initialized with Vulkan `1.1` core instance/device baseline)
- **Validation Layer**: `VK_LAYER_KHRONOS_validation` loaded directly from APK `lib/arm64-v8a/libVkLayer_khronos_validation.so` (`0x4000` page-aligned, `extractNativeLibs="false"`, `ZIP_STORED`) with `VK_EXT_debug_utils` messenger active
- **Optional Extensions (Runtime-Detected)**:
  - `VK_KHR_dynamic_rendering` (+ `VK_KHR_depth_stencil_resolve` + `VK_KHR_create_renderpass2`): **Supported & Enabled** (`dynamic_rendering=1`)
  - `VK_EXT_descriptor_indexing`: **Supported & Enabled** (`descriptor_indexing=1`)
- **Native ASTC LDR Formats**:
  - `VK_FORMAT_ASTC_4x4_UNORM_BLOCK` / `SRGB_BLOCK`: **Supported** (`astc_4x4=1`)
  - `VK_FORMAT_ASTC_6x6_UNORM_BLOCK` / `SRGB_BLOCK`: **Supported** (`astc_6x6=1`)
  - `VK_FORMAT_ASTC_8x8_UNORM_BLOCK` / `SRGB_BLOCK`: **Supported** (`astc_8x8=1`)
- **Swapchain**: `2340x1080` landscape, `4` images, `VK_FORMAT_R8G8B8A8_UNORM` (`format=37`), `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`, `VK_PRESENT_MODE_FIFO_KHR`

---

## 3. Dual Render Paths: `VkRenderPass` Fallback & `VK_KHR_dynamic_rendering`
To guarantee cross-vendor compatibility across older Vulkan 1.1 drivers while taking advantage of dynamic rendering on modern mobile GPUs:
1. **`RenderPath::RenderPassFallback` (Mandatory Baseline)**:
   - Creates a `VkRenderPass` (`initialLayout = VK_IMAGE_LAYOUT_UNDEFINED`, `finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`, `LOAD_OP_CLEAR`, `STORE_OP_STORE`), per-swapchain-image `VkFramebuffer`s, and a render-pass-bound `VkGraphicsPipeline`.
   - Always compiled and executed during startup self-test (`[M3_PASS] 3_RENDERPASS_FALLBACK: status=VERIFIED`).
2. **`RenderPath::DynamicRendering` (Fast Path)**:
   - When `caps().dynamic_rendering` is true, builds a second `VkGraphicsPipeline` chaining `VkPipelineRenderingCreateInfoKHR` (`renderPass = VK_NULL_HANDLE`) and records frames via `vkCmdBeginRenderingKHR` / `vkCmdEndRenderingKHR` with explicit `VkImageMemoryBarrier` transitions (`UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR`).
   - Verified during startup self-test (`[M3_PASS] 4_DYNAMIC_RENDERING: status=VERIFIED`) and used as the steady-state render path on Galaxy A56.

---

## 4. VMA 64 MiB Staging Ring-Buffer & Fence Synchronization
`StagingRingBuffer` (`src/vk_foundation/src/staging_ring.cpp`) manages a single persistent VMA buffer (`64 MiB` default, configurable up to `128 MiB`) allocated with `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT`:
- **Contiguous Wrap-Around**: Any allocation that would straddle the ring's tail boundary advances the monotonic write cursor to the next lap (`start_bucket + 1`) and increments `wrap_count_`.
- **Fence-Tracked Reclamation**: Every GPU copy submission records `{end_cursor, VkFence}` in a FIFO queue. When ring space is needed or fences complete, `notify_fence_completed()` / `vkWaitForFences()` advances `retired_cursor_` and increments `reclaim_count_`.
- **On-Device Self-Test**: Transfers `128 MiB` of data (`> 64 MiB` capacity) into a device-local buffer across alternating fences, completing with `total_MB=128 wraps=2 reclaims=9` and `0` validation warnings (`[M3_PASS] 5_VMA_STAGING_RING: capacity_mb=64 wrap_around=PASS fence_sync=PASS`).

---

## 5. Native ASTC Upload & Negative-Viewport Y-Flip Verification
- **Zero-RGBA8 ASTC Upload**: An `8x8` (`2x2` blocks = `64` bytes) `VK_FORMAT_ASTC_4x4_UNORM_BLOCK` texture with four distinct constant-color void-extent quadrants (Top-Left Red, Top-Right Green, Bottom-Left Blue, Bottom-Right Yellow) is staged through `StagingRingBuffer` and copied directly into an optimal-tiled `VkImage` (`[M3_PASS] 6_ASTC_DIRECT_UPLOAD: format=VK_FORMAT_ASTC_4x4_UNORM_BLOCK rgba8_decomp=0 upload_sample=PASS`).
- **Negative-Viewport Y-Flip (Vulkan 1.1 Core / `VK_KHR_maintenance1`)**:
  - Viewport is configured with `x = 0`, `y = H`, `width = W`, `height = -H` so Metal-style Y-up NDC coordinates (`Apex = (0.0, +0.5)`, `Bottom-Left = (-0.5, -0.5)`, `Bottom-Right = (+0.5, -0.5)`) map upright onto the screen while preserving Counter-Clockwise (`VK_FRONT_FACE_COUNTER_CLOCKWISE`) front-face winding with back-face culling (`VK_CULL_MODE_BACK_BIT`) enabled.
  - Automated screenshot analysis (`scripts/m3/verify_screencap.py` on `build/m3/m3_screencap.png`, `2340x1080`) verified:
    - Clear color `RGB(16, 48, 80)` ratio in central safe ROI: `0.784` (`>= 0.45`)
    - Non-clear ASTC triangle pixel ratio: `0.216` (`0.05..0.45`), chromatic `std = 127.1`
    - Upright Y-flip geometry: lower-half triangle pixel count vs upper-half pixel count = `237,506 / 78,394 = 3.03x` (`>= 1.5x`, matching the theoretical `3.0x` trapezoid-to-apex area ratio of an upright triangle)
    - Top-third vs bottom-third ASTC RGB mean Euclidean distance: `127.5` (`>= 20.0`)

---

## 6. Repeatable Verification Gate (`scripts/gate_m3.ps1`)
Run via:
```powershell
powershell -ExecutionPolicy Bypass -File scripts/gate_m3.ps1
```
Verified output on Samsung Galaxy A56 (`RRCY40086KE`):
- All 7 `[M3_PASS]` assertions present in `build/m3/m3_logcat.txt`
- `[M3_STATE] M3_SELFTEST_COMPLETE validation_errors=0 validation_warnings=0 clear_rgb=16,48,80 width=2340 height=1080`
- Screenshot pixel & Y-flip check passed (`build/m3/m3_screencap.png`)
- Exits `0` with `GATE M3: PASS`.
