#pragma once

#include "vk_foundation/vk_types.h"

#include <android/native_window.h>
#include <atomic>
#include <cstdint>
#include <vector>

namespace oh2::vk {

struct VkContextConfig {
    ANativeWindow* window = nullptr;
    bool enable_validation = true;
    const char* app_name = "OH2_M3_Vulkan";
};

class VkContext {
public:
    VkContext() = default;
    ~VkContext();

    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    bool init(const VkContextConfig& cfg);
    bool recreate_swapchain(ANativeWindow* window);
    void destroy();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkDevice device() const { return device_; }
    VkQueue graphics_queue() const { return graphics_queue_; }
    uint32_t graphics_queue_family() const { return graphics_queue_family_; }
    VkSurfaceKHR surface() const { return surface_; }
    VmaAllocator allocator() const { return allocator_; }

    VkSwapchainKHR swapchain() const { return swapchain_; }
    VkFormat swapchain_format() const { return swapchain_format_; }
    VkExtent2D swapchain_extent() const { return swapchain_extent_; }
    const std::vector<VkImage>& swapchain_images() const { return swapchain_images_; }
    const std::vector<VkImageView>& swapchain_image_views() const { return swapchain_image_views_; }

    const VkCapabilities& caps() const { return caps_; }

    PFN_vkCmdBeginRenderingKHR cmd_begin_rendering() const { return pfn_cmd_begin_rendering_; }
    PFN_vkCmdEndRenderingKHR cmd_end_rendering() const { return pfn_cmd_end_rendering_; }

    uint32_t validation_errors() const { return validation_errors_.load(std::memory_order_relaxed); }
    uint32_t validation_warnings() const { return validation_warnings_.load(std::memory_order_relaxed); }

    bool is_swapchain_srgb() const;

private:
    bool create_instance(const VkContextConfig& cfg);
    bool create_surface(ANativeWindow* window);
    bool pick_physical_device();
    bool create_device();
    bool create_allocator();
    bool create_swapchain(ANativeWindow* window);
    void destroy_swapchain();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
        void* user_data);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    PFN_vkCreateDebugUtilsMessengerEXT pfn_create_debug_utils_ = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pfn_destroy_debug_utils_ = nullptr;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_queue_family_ = UINT32_MAX;

    VmaAllocator allocator_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchain_color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchain_extent_{0, 0};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    VkCapabilities caps_{};

    PFN_vkCmdBeginRenderingKHR pfn_cmd_begin_rendering_ = nullptr;
    PFN_vkCmdEndRenderingKHR pfn_cmd_end_rendering_ = nullptr;

    std::atomic<uint32_t> validation_errors_{0};
    std::atomic<uint32_t> validation_warnings_{0};
};

}  // namespace oh2::vk
