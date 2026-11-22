# M5 Metal-to-Vulkan Shim, Android Platform Bridges & Live Integration (`M5_METAL_VULKAN`)

## 1. Architecture Overview
Milestone M5 integrates the M1 Mach-O ARM64 Loader (`src/loader/`), M2 Objective-C Runtime & Darwin POSIX Translation Layer (`src/runtime/`, `src/libc_wrapper/`), M3 Vulkan 1.1 Graphics Foundation (`src/vk_foundation/`), and M4 Offline AIR 2.0 -> SPIR-V 1.3 Shader Cache (`build/shaders/spirv_cache.bin`, `4,482` unique `MTLB` entries / `4,544` total shaders, `69,090,604` B) into the primary signed Android application **`com.oceanhorn2.port`** (`build/m5/oh2_android-debug.apk`).

| Subsystem | Source Files | Responsibility |
|---|---|---|
| **Metal Device, Queue, CommandBuffer & Library** | `src/metal_vk/mtl_vk_device.cpp`, `src/metal_vk/mtl_vk_internal.h` | `_MTLCreateSystemDefaultDevice`, `MTLDevice`, `MTLCommandQueue`, `MTLCommandBuffer` (`addScheduledHandler:`, `addCompletedHandler:`, `presentDrawable:`, `commit`, `waitUntilCompleted`, 16 MiB per-frame inline `setVertexBytes:`/`setFragmentBytes:` ring), `MTLLibrary` (`newLibraryWithData:error:` scanning `FMetalCodeHeader` + `MTLB` SHA-256 + `NAME` tag fallback), `MTLFunction`, `MTLFence`, `MTLHeap`, `MTLCaptureManager` |
| **Metal Resources & CAMetalLayer** | `src/metal_vk/mtl_vk_resources.cpp` | `MTLBuffer` (persistent host-coherent VMA mapped memory), `MTLTexture` & `MTLTextureDescriptor` (native ASTC 4x4/6x6/8x8 LDR + depth/stencil aspect view specialization + `MTLPixelFormatA8Unorm -> VK_FORMAT_R8_UNORM` swizzle `(0,0,0,R)`), `MTLSamplerState`, `CAMetalLayer`, `CAMetalDrawable` |
| **Metal Pipeline States & Descriptors** | `src/metal_vk/mtl_vk_pipeline.cpp` | `MTLRenderPipelineDescriptor`, `MTLRenderPipelineColorAttachmentDescriptorArray` (`objectAtIndexedSubscript:` + `setObject:atIndexedSubscript:`), `MTLVertexDescriptor`, `MTLDepthStencilDescriptor`, `MTLRenderPassDescriptor` (with `init` and `copyWithZone:` deep copies), specialized `VkPipeline` cache (dynamic rendering + `VkRenderPass` fallback), Apple `MTLColorWriteMask` bit reversal (`Red=0x8, Green=0x4, Blue=0x2, Alpha=0x1`), negative-viewport front-face winding preservation (`CW -> VK_FRONT_FACE_CLOCKWISE`) |
| **Metal Command Encoders & Framebuffer Fetch** | `src/metal_vk/mtl_vk_encoder.cpp` | `MTLRenderCommandEncoder`, `MTLComputeCommandEncoder`, `MTLBlitCommandEncoder`, dynamic viewport/scissor/blend-constants/stencil-ref/depth-bias, per-draw descriptor set allocation (`Set 0` VS/CS, `Set 1` PS), and **in-pass `color(0)` Framebuffer Fetch snapshot** (`SPIRV_ENTRY_FLAG_FB_FETCH = 1 << 5`, binding synthetic `metal_slot = 30` / Set 1 Binding 62) |
| **F10 Isolated Self-Test Suite** | `src/metal_vk/mtl_vk_selftest.cpp` | 5 deterministic GPU self-tests (`metal_objects`, `metal_render_pass`, `encoder_scissor_load`, `metal_compute`, `swapchain_resize_3x`) with `VK_LAYER_KHRONOS_validation` enabled |
| **Android Platform Bridges & NativeActivity** | `src/platform/{audio_bridge,touch_bridge,vsync_bridge}.cpp`, `android/app/src/oh2_android_main.cpp` | `AudioUnit` / `AUGraph` (`44,100` Hz 16-bit mono/stereo PCM across 8 buses) -> `AAudio` stereo float mixer with automatic lifecycle pause/resume (`APP_CMD_PAUSE`/`LOST_FOCUS`/`STOP`), `AInputEvent` -> `UITouch`/`UIEvent` main-thread delivery, `AChoreographer` -> `CADisplayLink` VSYNC pump, and sticky immersive fullscreen (`WindowInsetsController` + `setSystemUiVisibility(0x1706)`) |

---

## 2. Root-Cause Analysis & Key Fixes

### 2.1 Non-Retained Metal Descriptor Ownership, `copyWithZone:`, and Apple `MTLColorWriteMask` Bit Order (UI, Logo, Button & Font Alpha Blending)
- **Symptom**: During initial bringup, the `OCEANHORN 2` title logo, `PRESS TO START` banner, menu buttons, diamond icons, and font glyphs rendered inside solid white or black rectangles (`blended_psos = 0`).
- **Root Causes**:
  1. `FMetalStateCache` in UE4.23 allocates temporary Objective-C descriptors (`[[MTLRenderPipelineDescriptor alloc] init]`, `[[MTLDepthStencilDescriptor alloc] init]`, `[[MTLVertexDescriptor alloc] init]`), copies them via `-[NSObject copy]` / `copyWithZone:`, and assigns color attachments via `setObject:atIndexedSubscript:`. Without dedicated `init` and `copyWithZone:` methods on all Metal descriptor classes, `alloc_instance` left `impl == nullptr` or shallow-copied pointers, causing all `setBlendingEnabled:YES` and `setSourceRGBBlendFactor:` calls to be silently dropped.
  2. In Apple Metal (`MTLRenderPipeline.h`), `MTLColorWriteMask` bit positions are reversed relative to Vulkan `VkColorComponentFlags`:
     - Metal: `MTLColorWriteMaskRed = 0x8 (1 << 3)`, `Green = 0x4 (1 << 2)`, `Blue = 0x2 (1 << 1)`, `Alpha = 0x1 (1 << 0)`.
     - Vulkan: `VK_COLOR_COMPONENT_R_BIT = 0x1`, `G = 0x2`, `B = 0x4`, `A = 0x8`.
     Passing the raw 4-bit mask without swapping `R <-> A` and `G <-> B` caused single-channel masks (such as `MTLColorWriteMaskAlpha` or `MTLColorWriteMaskRed`) to write into the wrong channel.
- **Fix**: Implemented owned `init`, `copy`, `copyWithZone:`, `reset`, and `setObject:atIndexedSubscript:` across all Metal descriptors in `src/metal_vk/mtl_vk_pipeline.cpp` and `src/metal_vk/mtl_vk_resources.cpp`, translated `MTLColorWriteMask` bit positions accurately, and wired `vkCmdSetBlendConstants`. Result: `blended_psos` jumped from `0` to `34 / 304` and all UI logos, fonts, buttons, and icons composite cleanly.

### 2.2 Metal Programmable Blending / Framebuffer Fetch (`air.render_target` -> `OpImageFetch`) — Root Cause of Black 3D Scene Background
- **Symptom**: After UI alpha blending was fixed, the 2D menus (`Title Screen`, `Save Slots`, `Options`) rendered crisply, but the 3D Outcast Island scene behind the menus remained black.
- **Root Cause**:
  - Using an in-engine Frame #500 GPU readback and scanline probe across all 19 render passes (`RP#1..RP#19`), we proved that in `RP#4` (the `2340x1080` `VK_FORMAT_R16G16B16A16_SFLOAT` + `VK_FORMAT_D32_SFLOAT_S8_UINT` 3D Base Pass), draws `D#0..D#120` rendered the full 3D Outcast Island scene across 100% of the screen (`nz_d = 2340 / 2340` pixels with valid scene depths `0.000033 .. 0.000554`).
  - However, the final draw of `RP#4` (`D#121`, `vs = Main_000007ba_6e442da6`, `ps = Main_0000053e_e1647fe1`, fullscreen triangle `icnt = 3`, `dCmp = Always`, `dWr = 0`, `blending = 0`, `write_mask = 0xf`) is UE4's Mobile HDR in-pass tone-compression subpass shader:
    $$\text{rgb}' = \min(511.0, \text{inColor.rgb} \times \text{Exposure}), \quad \text{outColor.rgb} = \frac{\text{rgb}'}{1.0 + \max(r', g', b')}, \quad \text{outColor.a} = \text{inColor.a}$$
  - On iOS Metal, `Main_0000053e_e1647fe1` reads the current pixel of `color(0)` directly from tile memory via programmable blending (`inColor [[color(0)]]`, encoded in AIR 2.0 metadata as an input argument with `kind == "air.render_target"`).
  - Because `tools/air2spirv/air_to_spirv.py` previously lowered `kind == "air.render_target"` input arguments to `OpConstantNull %v4f32` (`vec4(0, 0, 0, 0)`), `RP#4 D#121` overwrote all `2340x1080` pixels of `c0` with `(0, 0, 0, 0)` at the end of every 3D base pass.
- **Fix**:
  1. Updated `tools/air2spirv/air_to_spirv.py` so any fragment shader input with `kind == "air.render_target"` sets `SPIRV_ENTRY_FLAG_FB_FETCH = (1 << 5)`, binds synthetic 2D texture slot `30` (Vulkan `DescriptorSet 1`, `Binding 62`), converts `gl_FragCoord.xy` to integer texel coordinates, and emits `OpImageFetch` at `Lod 0`. Rebuilt and re-validated all `4,544 / 4,544` shaders (`100.00%` `spirv-val --target-env vulkan1.1` pass rate).
  2. Updated `src/metal_vk/mtl_vk_encoder.cpp` (`snapshot_color0_for_fb_fetch`) so that before any draw call whose fragment shader has `SPIRV_ENTRY_FLAG_FB_FETCH` set, the active render pass suspends, copies `color[0]` into a cached GPU texture (`st.fb_fetch_image` / `st.fb_fetch_view`), transitions it to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`, binds it at `re->ps.tex_view[30]`, and resumes the render pass with `VK_ATTACHMENT_LOAD_OP_LOAD` on all attachments.
  3. Also corrected negative-viewport winding in `src/metal_vk/mtl_vk_pipeline.cpp` (`kMTLWindingCW -> VK_FRONT_FACE_CLOCKWISE` because `VK_KHR_maintenance1` negative viewport height preserves screen-space winding) and depth/stencil texture view aspect masks in `src/metal_vk/mtl_vk_resources.cpp`.

### 2.3 Android Lifecycle Audio Pause/Resume & Sticky Immersive Fullscreen
- **Background Audio Leak Fix**: Added `oh2_platform_audio_pause()` and `oh2_platform_audio_resume()` in `src/platform/audio_bridge.cpp` (`AAudioStream_requestPause` + `AAudioStream_requestFlush` + atomic `g_audio_paused` gate), wired to `APP_CMD_PAUSE`, `APP_CMD_LOST_FOCUS`, `APP_CMD_STOP`, and `APP_CMD_TERM_WINDOW` in `android/app/src/oh2_android_main.cpp`. Pressing the Android Home button immediately pauses AAudio output (`OH2_AUDIO: AAudio stream paused (app backgrounded/unfocused)`).
- **Navigation Bar Auto-Hide (Sticky Immersive Mode)**: Chained `ANativeActivityCallbacks` (`onWindowFocusChanged`, `onResume`, `onNativeWindowCreated`) on the Android main UI thread in `android/app/src/oh2_android_main.cpp` to invoke `Window.setDecorFitsSystemWindows(false)`, `WindowInsetsController.hide(statusBars | navigationBars | captionBar)`, `setSystemBarsBehavior(BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE)`, display cutout mode `ALWAYS`, and `decorView.setSystemUiVisibility(0x1706)`.
- **Swapchain Suboptimal Coalescing**: Avoided recreating the Vulkan swapchain on `VK_SUBOPTIMAL_KHR` every frame (`st.swapchain_out_of_date` is now triggered only on `VK_ERROR_OUT_OF_DATE_KHR` or `APP_CMD_WINDOW_RESIZED`), eliminating `vkDeviceWaitIdle` stalls and ensuring immediate touch input responsiveness.

---

## 3. On-Device Verification Results (`SM-A566B` / `RRCY40086KE`)

### 3.1 F10 Isolated Self-Test Suite (`scripts/gate_m5.ps1 --test <name>`)
All 5 isolated Metal-to-Vulkan self-tests executed on Samsung Galaxy A56 (`Samsung Xclipse 540`) with `VK_LAYER_KHRONOS_validation` enabled:

| F10 Test Name | Verified Scope | Validation Errors / Warnings | Result |
|---|---|---:|---|
| `metal_objects` | `_MTLCreateSystemDefaultDevice`, `MTLCommandQueue`, `MTLBuffer` allocation & CPU/GPU mapping, `MTLTexture`, `MTLSamplerState`, `MTLLibrary` (`spirv_cache.bin` lookup) | `0 / 0` | **PASS** |
| `metal_render_pass` | Offscreen `MTLRenderCommandEncoder` clear + triangle draw + GPU readback pixel verification | `0 / 0` | **PASS** |
| `encoder_scissor_load` | `MTLLoadActionClear` followed by `MTLLoadActionLoad` with `setScissorRect:` quadrant preservation | `0 / 0` | **PASS** |
| `metal_compute` | `MTLComputeCommandEncoder` dispatch (`Global` compute shader) + SSBO output verification | `0 / 0` | **PASS** |
| `swapchain_resize_3x` | 3 consecutive `mtl_vk_set_surface` swapchain recreations + present synchronization | `0 / 0` | **PASS** |

### 3.2 F11 Live Oceanhorn 2 Boot, Menu & 3D Gameplay Verification (`scripts/gate_m5.ps1`)
- **APK Packaging & 16 KiB Alignment**:
  - `build/m5/oh2_android-debug.apk` (`95,935,044` B) packaged with app icon mipmaps extracted from `Payload/OH2.app/AppIcon60x60@3x.png`.
  - ZIP entry alignment verified (`mod 16384 == 0`):
    - `lib/arm64-v8a/liboh2_android.so`: offset `0x24000` (`mod16k = 0`)
    - `lib/arm64-v8a/libVkLayer_khronos_validation.so`: offset `0xf1c000` (`mod16k = 0`)
    - `assets/oh2_macho.bin`: offset `0x2840000` (`mod16k = 0`)
- **Live Gameplay Telemetry (`Present frame #1 .. #8100+`)**:
  - Booted unmodified `OH2` Mach-O binary (`53,729,072` B), mounted `oh2-ios.pak` (`2,430,963,623` B), loaded `spirv_cache.bin` (`4,482` unique entries), and reached:
    1. **Splash Screen** (`FRAME non_black=0.9991 max_std=68.21`).
    2. **Title Screen** (`OCEANHORN 2: KNIGHTS OF THE LOST REALM` + `PRESS TO START` over live 3D Outcast Island water, island shadow, sky, and clouds).
    3. **Save Slots & Offline Dialog** (3D Outcast Island scene visible behind translucent UI panels).
    4. **Live 3D Gameplay on Outcast Island** (`Find Master Mayfair's Package in Pirate's Hideout`): walked along the wooden dock onto the sandy beach, rotated the 3D camera, triggered action/attack buttons, and rendered 3D terrain, water reflections, foliage, HUD hearts, 3D minimap, and controller prompts with **`mtlb_hits = 430, misses = 0`** (`100%` shader cache hit rate, `304` specialized graphics PSOs, `34` blended PSOs).
  - **Audio**: `AAudio output stream started: req=44100 Hz -> hw=44100 Hz, 2 ch (PCM_FLOAT), ue4_src=44100 Hz`, automatically paused on `KEYCODE_HOME` (`AAudio stream paused (app backgrounded/unfocused)`).
  - **Memory**: Live Metal allocations during 3D gameplay remained bounded at `bufs = 1,107 (57 MiB)`, `tex = 2,217 (650 MiB)` (`0` CPU-side ASTC RGBA8 decompressions).
