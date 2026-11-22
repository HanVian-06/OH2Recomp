#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace oh2::vk {

enum class RenderPath {
    RenderPassFallback = 0,
    DynamicRendering = 1,
};

enum class RenderPathPreference {
    Auto = 0,
    DynamicRenderingOnly = 1,
    RenderPassFallbackOnly = 2,
};

struct VkCapabilities {
    uint32_t api_version = 0;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};

    bool validation_layer_enabled = false;
    bool debug_utils_enabled = false;

    bool has_dynamic_rendering_exts = false;
    bool dynamic_rendering = false;

    bool has_descriptor_indexing_ext = false;
    bool descriptor_indexing = false;
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptor_indexing_features{};

    bool astc_ldr_feature = false;
    bool astc_4x4 = false;
    bool astc_6x6 = false;
    bool astc_8x8 = false;
    bool astc_4x4_srgb = false;
    bool astc_6x6_srgb = false;
    bool astc_8x8_srgb = false;

    VkDeviceSize optimal_buffer_copy_offset_alignment = 1;
    VkDeviceSize non_coherent_atom_size = 128;
};

struct Vertex {
    float pos[3];
    float uv[2];
};

inline const char* render_path_name(RenderPath path) {
    return path == RenderPath::DynamicRendering ? "dynamic_rendering" : "renderpass_fallback";
}

}  // namespace oh2::vk
