#ifndef OH2_METAL_VK_H
#define OH2_METAL_VK_H

#include <android/native_window.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtlVkStats {
    uint64_t draw_calls;
    uint64_t indexed_draw_calls;
    uint64_t compute_dispatches;
    uint64_t blit_commands;
    uint64_t command_buffers_committed;
    uint64_t presents;
    uint64_t graphics_pipelines_created;
    uint64_t compute_pipelines_created;
    uint64_t astc_textures_created;
    uint64_t astc_cpu_decompress_count;
    uint64_t surface_recreates;
    uint32_t validation_errors;
    uint32_t validation_warnings;
    uint32_t spirv_cache_entries;
    uint32_t dynamic_rendering_active;
} MtlVkStats;

/* Initialize Vulkan context, StagingRingBuffer, SPIR-V cache, and dummy fallback bindings */
int  mtl_vk_init(ANativeWindow *window, const char *spirv_cache_path, int enable_validation);

/* Recreate or rebind Vulkan surface & swapchain after Android window resize/surface recreation */
int  mtl_vk_set_surface(ANativeWindow *window);

/* Pause rendering and cleanly destroy swapchain/surface when Android destroys the window (Home/background) */
void mtl_vk_pause_surface(void);

/* Unblock any render thread waiting on a paused surface before shutdown */
void mtl_vk_begin_shutdown(void);

/* Release all Vulkan resources */
void mtl_vk_shutdown(void);

/* Register all Metal / CAMetalLayer ObjC classes, methods, and _MTLCreateSystemDefaultDevice override */
void mtl_vk_register_objc_bridges(void);

/* Snapshot runtime telemetry & Vulkan validation counts */
void mtl_vk_get_stats(MtlVkStats *out_stats);

/* Run one of the 5 F10 M5 self-tests (logs "[M5_TEST] PASS: <name>" or FAIL); returns 0 on PASS */
int  mtl_vk_run_selftest(const char *test_name, ANativeWindow *window, const char *spirv_cache_path);

#ifdef __cplusplus
}
#endif

#endif /* OH2_METAL_VK_H */
