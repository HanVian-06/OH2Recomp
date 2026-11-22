#include "vk_foundation/vk_context.h"
#include "vk_foundation/vk_log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace oh2::vk {

namespace {

bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool has_layer(const std::vector<VkLayerProperties>& layers, const char* name) {
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool supports_astc_format(VkPhysicalDevice phys, VkFormat fmt) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(phys, fmt, &props);
    constexpr VkFormatFeatureFlags kReq =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (props.optimalTilingFeatures & kReq) == kReq;
}

}  // namespace

VkContext::~VkContext() {
    destroy();
}

VKAPI_ATTR VkBool32 VKAPI_CALL VkContext::debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void* user_data) {
    auto* self = static_cast<VkContext*>(user_data);
    int prio = ANDROID_LOG_INFO;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        prio = ANDROID_LOG_ERROR;
        if (self) {
            self->validation_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        prio = ANDROID_LOG_WARN;
        if (self) {
            self->validation_warnings_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const char* id_name = (callback_data && callback_data->pMessageIdName)
                              ? callback_data->pMessageIdName
                              : "unknown";
    const char* msg = (callback_data && callback_data->pMessage)
                          ? callback_data->pMessage
                          : "(null)";
    __android_log_print(prio, OH2_VK_VAL_TAG, "[type=0x%x][%s] %s",
                        static_cast<unsigned>(type), id_name, msg);
    return VK_FALSE;
}

bool VkContext::init(const VkContextConfig& cfg) {
    destroy();
    if (!cfg.window) {
        M3_LOGE("VkContext::init failed: null ANativeWindow");
        return false;
    }

    if (!create_instance(cfg)) return false;
    if (!create_surface(cfg.window)) return false;
    if (!pick_physical_device()) return false;
    if (!create_device()) return false;
    if (!create_allocator()) return false;
    if (!create_swapchain(cfg.window)) return false;
    return true;
}

bool VkContext::recreate_swapchain(ANativeWindow* window) {
    if (!device_ || !window) return false;
    vkDeviceWaitIdle(device_);
    destroy_swapchain();
    return create_swapchain(window);
}

void VkContext::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    destroy_swapchain();

    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (debug_messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE && pfn_destroy_debug_utils_) {
        pfn_destroy_debug_utils_(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    physical_device_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
    graphics_queue_family_ = UINT32_MAX;
    pfn_cmd_begin_rendering_ = nullptr;
    pfn_cmd_end_rendering_ = nullptr;
    caps_ = {};
}

bool VkContext::create_instance(const VkContextConfig& cfg) {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> avail_layers(layer_count);
    if (layer_count > 0) {
        vkEnumerateInstanceLayerProperties(&layer_count, avail_layers.data());
    }

    std::vector<const char*> enabled_layers;
    constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
    if (cfg.enable_validation && has_layer(avail_layers, kValidationLayer)) {
        enabled_layers.push_back(kValidationLayer);
        caps_.validation_layer_enabled = true;
    } else if (cfg.enable_validation) {
        M3_LOGE("Requested %s not found in vkEnumerateInstanceLayerProperties (count=%u)",
                kValidationLayer, layer_count);
        return false;
    }

    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> avail_exts(ext_count);
    if (ext_count > 0) {
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, avail_exts.data());
    }

    // Also include extensions exposed by validation layer if enabled.
    if (caps_.validation_layer_enabled) {
        uint32_t layer_ext_count = 0;
        vkEnumerateInstanceExtensionProperties(kValidationLayer, &layer_ext_count, nullptr);
        if (layer_ext_count > 0) {
            std::vector<VkExtensionProperties> layer_exts(layer_ext_count);
            vkEnumerateInstanceExtensionProperties(kValidationLayer, &layer_ext_count, layer_exts.data());
            avail_exts.insert(avail_exts.end(), layer_exts.begin(), layer_exts.end());
        }
    }

    if (!has_extension(avail_exts, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !has_extension(avail_exts, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        M3_LOGE("Missing required WSI surface instance extensions");
        return false;
    }

    std::vector<const char*> enabled_exts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };

    if (has_extension(avail_exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        enabled_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        caps_.debug_utils_enabled = true;
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = cfg.app_name ? cfg.app_name : "OH2_M3_Vulkan";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app_info.pEngineName = "OH2_VK_Foundation";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkDebugUtilsMessengerCreateInfoEXT debug_info{};
    debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debug_info.pfnUserCallback = &VkContext::debug_callback;
    debug_info.pUserData = this;

    VkInstanceCreateInfo inst_info{};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledLayerCount = static_cast<uint32_t>(enabled_layers.size());
    inst_info.ppEnabledLayerNames = enabled_layers.empty() ? nullptr : enabled_layers.data();
    inst_info.enabledExtensionCount = static_cast<uint32_t>(enabled_exts.size());
    inst_info.ppEnabledExtensionNames = enabled_exts.data();
    if (caps_.debug_utils_enabled) {
        inst_info.pNext = &debug_info;
    }

    VkResult res = vkCreateInstance(&inst_info, nullptr, &instance_);
    if (res != VK_SUCCESS) {
        M3_LOGE("vkCreateInstance failed: %d", static_cast<int>(res));
        return false;
    }

    if (caps_.debug_utils_enabled) {
        pfn_create_debug_utils_ = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        pfn_destroy_debug_utils_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (!pfn_create_debug_utils_ || !pfn_destroy_debug_utils_) {
            M3_LOGE("VK_EXT_debug_utils enabled but messenger procs failed to load");
            return false;
        }
        res = pfn_create_debug_utils_(instance_, &debug_info, nullptr, &debug_messenger_);
        if (res != VK_SUCCESS) {
            M3_LOGE("vkCreateDebugUtilsMessengerEXT failed: %d", static_cast<int>(res));
            return false;
        }
    }

    M3_LOGI("Instance created (Vulkan 1.1 core). validation=%d debug_utils=%d",
            caps_.validation_layer_enabled ? 1 : 0,
            caps_.debug_utils_enabled ? 1 : 0);
    return true;
}

bool VkContext::create_surface(ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR surf_info{};
    surf_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surf_info.window = window;

    VkResult res = vkCreateAndroidSurfaceKHR(instance_, &surf_info, nullptr, &surface_);
    if (res != VK_SUCCESS) {
        M3_LOGE("vkCreateAndroidSurfaceKHR failed: %d", static_cast<int>(res));
        return false;
    }
    return true;
}

bool VkContext::pick_physical_device() {
    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance_, &dev_count, nullptr);
    if (dev_count == 0) {
        M3_LOGE("No Vulkan physical devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(dev_count);
    vkEnumeratePhysicalDevices(instance_, &dev_count, devices.data());

    for (VkPhysicalDevice phys : devices) {
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qfs.data());

        uint32_t chosen_qf = UINT32_MAX;
        for (uint32_t i = 0; i < qf_count; ++i) {
            if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
            VkBool32 present_supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface_, &present_supported);
            if (present_supported == VK_TRUE) {
                chosen_qf = i;
                break;
            }
        }
        if (chosen_qf == UINT32_MAX) continue;

        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> dev_exts(ext_count);
        if (ext_count > 0) {
            vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, dev_exts.data());
        }
        if (!has_extension(dev_exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        physical_device_ = phys;
        graphics_queue_family_ = chosen_qf;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        caps_.api_version = props.apiVersion;
        caps_.vendor_id = props.vendorID;
        caps_.device_id = props.deviceID;
        std::memcpy(caps_.device_name, props.deviceName, sizeof(caps_.device_name));
        caps_.optimal_buffer_copy_offset_alignment =
            std::max<VkDeviceSize>(1, props.limits.optimalBufferCopyOffsetAlignment);
        caps_.non_coherent_atom_size =
            std::max<VkDeviceSize>(1, props.limits.nonCoherentAtomSize);

        caps_.has_dynamic_rendering_exts =
            has_extension(dev_exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) &&
            has_extension(dev_exts, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) &&
            has_extension(dev_exts, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);

        caps_.has_descriptor_indexing_ext =
            has_extension(dev_exts, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);

        // Query Vulkan 1.1 core VkPhysicalDeviceFeatures2 + extension feature structs.
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VkPhysicalDeviceDynamicRenderingFeaturesKHR dyn_features{};
        dyn_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;

        VkPhysicalDeviceDescriptorIndexingFeaturesEXT desc_idx_features{};
        desc_idx_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;

        void** tail = &features2.pNext;
        if (caps_.has_dynamic_rendering_exts) {
            *tail = &dyn_features;
            tail = &dyn_features.pNext;
        }
        if (caps_.has_descriptor_indexing_ext) {
            *tail = &desc_idx_features;
            tail = &desc_idx_features.pNext;
        }

        vkGetPhysicalDeviceFeatures2(phys, &features2);

        caps_.astc_ldr_feature = (features2.features.textureCompressionASTC_LDR == VK_TRUE);
        caps_.dynamic_rendering =
            caps_.has_dynamic_rendering_exts && (dyn_features.dynamicRendering == VK_TRUE);
        caps_.descriptor_indexing_features = desc_idx_features;
        caps_.descriptor_indexing =
            caps_.has_descriptor_indexing_ext &&
            (desc_idx_features.descriptorBindingPartiallyBound == VK_TRUE ||
             desc_idx_features.runtimeDescriptorArray == VK_TRUE);

        caps_.astc_4x4 = supports_astc_format(phys, VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
        caps_.astc_6x6 = supports_astc_format(phys, VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
        caps_.astc_8x8 = supports_astc_format(phys, VK_FORMAT_ASTC_8x8_UNORM_BLOCK);
        caps_.astc_4x4_srgb = supports_astc_format(phys, VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
        caps_.astc_6x6_srgb = supports_astc_format(phys, VK_FORMAT_ASTC_6x6_SRGB_BLOCK);
        caps_.astc_8x8_srgb = supports_astc_format(phys, VK_FORMAT_ASTC_8x8_SRGB_BLOCK);

        M3_LOGI("GPU=%s api=0x%x vendor=0x%x dev=0x%x dyn_render=%d desc_idx=%d astc=(%d,%d,%d)",
                caps_.device_name, caps_.api_version, caps_.vendor_id, caps_.device_id,
                caps_.dynamic_rendering ? 1 : 0,
                caps_.descriptor_indexing ? 1 : 0,
                caps_.astc_4x4 ? 1 : 0,
                caps_.astc_6x6 ? 1 : 0,
                caps_.astc_8x8 ? 1 : 0);
        return true;
    }

    M3_LOGE("No physical device supports graphics+present+swapchain");
    return false;
}

bool VkContext::create_device() {
    float queue_prio = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = graphics_queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_prio;

    std::vector<const char*> dev_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (caps_.dynamic_rendering) {
        dev_exts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        dev_exts.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        dev_exts.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    }
    if (caps_.has_descriptor_indexing_ext) {
        dev_exts.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }

    VkPhysicalDeviceFeatures enabled_core{};
    if (caps_.astc_ldr_feature) {
        enabled_core.textureCompressionASTC_LDR = VK_TRUE;
    }

    VkPhysicalDeviceDynamicRenderingFeaturesKHR enabled_dyn{};
    enabled_dyn.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    enabled_dyn.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceDescriptorIndexingFeaturesEXT enabled_desc_idx = caps_.descriptor_indexing_features;
    enabled_desc_idx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    enabled_desc_idx.pNext = nullptr;

    VkDeviceCreateInfo dev_info{};
    dev_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_info.queueCreateInfoCount = 1;
    dev_info.pQueueCreateInfos = &queue_info;
    dev_info.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
    dev_info.ppEnabledExtensionNames = dev_exts.data();
    dev_info.pEnabledFeatures = &enabled_core;

    const void* pnext_head = nullptr;
    if (caps_.dynamic_rendering && caps_.has_descriptor_indexing_ext) {
        enabled_dyn.pNext = &enabled_desc_idx;
        pnext_head = &enabled_dyn;
    } else if (caps_.dynamic_rendering) {
        pnext_head = &enabled_dyn;
    } else if (caps_.has_descriptor_indexing_ext) {
        pnext_head = &enabled_desc_idx;
    }
    dev_info.pNext = pnext_head;

    VkResult res = vkCreateDevice(physical_device_, &dev_info, nullptr, &device_);
    if (res != VK_SUCCESS) {
        M3_LOGE("vkCreateDevice failed: %d", static_cast<int>(res));
        return false;
    }

    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);

    if (caps_.dynamic_rendering) {
        pfn_cmd_begin_rendering_ = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
            vkGetDeviceProcAddr(device_, "vkCmdBeginRenderingKHR"));
        pfn_cmd_end_rendering_ = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
            vkGetDeviceProcAddr(device_, "vkCmdEndRenderingKHR"));
        if (!pfn_cmd_begin_rendering_ || !pfn_cmd_end_rendering_) {
            M3_LOGW("Dynamic rendering procs unavailable; disabling dynamic rendering path");
            caps_.dynamic_rendering = false;
        }
    }
    return true;
}

bool VkContext::create_allocator() {
    VmaVulkanFunctions vma_funcs{};
    vma_funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vma_funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.physicalDevice = physical_device_;
    alloc_info.device = device_;
    alloc_info.instance = instance_;
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_1;
    alloc_info.pVulkanFunctions = &vma_funcs;

    VkResult res = vmaCreateAllocator(&alloc_info, &allocator_);
    if (res != VK_SUCCESS) {
        M3_LOGE("vmaCreateAllocator failed: %d", static_cast<int>(res));
        return false;
    }
    return true;
}

bool VkContext::create_swapchain(ANativeWindow* window) {
    VkSurfaceCapabilitiesKHR surf_caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &surf_caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
    if (fmt_count == 0) {
        M3_LOGE("No surface formats available");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    bool found_unorm = false;
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            found_unorm = true;
            break;
        }
    }
    if (!found_unorm) {
        for (const auto& f : formats) {
            if ((f.format == VK_FORMAT_R8G8B8A8_SRGB || f.format == VK_FORMAT_B8G8R8A8_SRGB) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
        }
    }

    swapchain_format_ = chosen.format;
    swapchain_color_space_ = chosen.colorSpace;

    if (surf_caps.currentExtent.width != UINT32_MAX &&
        surf_caps.currentExtent.height != UINT32_MAX) {
        swapchain_extent_ = surf_caps.currentExtent;
    } else {
        uint32_t w = static_cast<uint32_t>(std::max(1, ANativeWindow_getWidth(window)));
        uint32_t h = static_cast<uint32_t>(std::max(1, ANativeWindow_getHeight(window)));
        swapchain_extent_.width = std::clamp(w, surf_caps.minImageExtent.width, surf_caps.maxImageExtent.width);
        swapchain_extent_.height = std::clamp(h, surf_caps.minImageExtent.height, surf_caps.maxImageExtent.height);
    }

    uint32_t img_count = std::max(surf_caps.minImageCount, 2u);
    if (surf_caps.maxImageCount > 0 && img_count > surf_caps.maxImageCount) {
        img_count = surf_caps.maxImageCount;
    }

    VkSurfaceTransformFlagBitsKHR pre_transform = surf_caps.currentTransform;
    if (surf_caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alpha_candidates[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    for (auto cand : alpha_candidates) {
        if (surf_caps.supportedCompositeAlpha & cand) {
            composite_alpha = cand;
            break;
        }
    }

    VkSwapchainCreateInfoKHR sc_info{};
    sc_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc_info.surface = surface_;
    sc_info.minImageCount = img_count;
    sc_info.imageFormat = swapchain_format_;
    sc_info.imageColorSpace = swapchain_color_space_;
    sc_info.imageExtent = swapchain_extent_;
    sc_info.imageArrayLayers = 1;
    sc_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_info.preTransform = pre_transform;
    sc_info.compositeAlpha = composite_alpha;
    sc_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc_info.clipped = VK_TRUE;

    VkResult res = vkCreateSwapchainKHR(device_, &sc_info, nullptr, &swapchain_);
    if (res != VK_SUCCESS) {
        M3_LOGE("vkCreateSwapchainKHR failed: %d", static_cast<int>(res));
        return false;
    }

    uint32_t sc_img_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &sc_img_count, nullptr);
    swapchain_images_.resize(sc_img_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &sc_img_count, swapchain_images_.data());

    swapchain_image_views_.resize(sc_img_count, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < sc_img_count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = swapchain_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain_format_;
        view_info.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
        };
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        res = vkCreateImageView(device_, &view_info, nullptr, &swapchain_image_views_[i]);
        if (res != VK_SUCCESS) {
            M3_LOGE("vkCreateImageView[%u] failed: %d", i, static_cast<int>(res));
            return false;
        }
    }

    M3_LOGI("Swapchain created: %ux%u images=%u format=%d preTransform=0x%x",
            swapchain_extent_.width, swapchain_extent_.height,
            sc_img_count, static_cast<int>(swapchain_format_),
            static_cast<unsigned>(pre_transform));
    return true;
}

void VkContext::destroy_swapchain() {
    for (VkImageView view : swapchain_image_views_) {
        if (view != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    swapchain_image_views_.clear();
    swapchain_images_.clear();

    if (swapchain_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VkContext::is_swapchain_srgb() const {
    return swapchain_format_ == VK_FORMAT_R8G8B8A8_SRGB ||
           swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB;
}

}  // namespace oh2::vk
