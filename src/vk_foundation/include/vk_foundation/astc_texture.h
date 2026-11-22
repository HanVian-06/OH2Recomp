#pragma once

#include "vk_foundation/staging_ring.h"
#include "vk_foundation/vk_context.h"

#include <cstddef>
#include <cstdint>

namespace oh2::vk {

class AstcTexture {
public:
    AstcTexture() = default;
    ~AstcTexture();

    AstcTexture(const AstcTexture&) = delete;
    AstcTexture& operator=(const AstcTexture&) = delete;

    static bool is_format_supported(const VkContext& ctx, VkFormat format);

    bool create_and_upload(VkContext& ctx,
                           StagingRingBuffer& ring,
                           VkCommandPool cmd_pool,
                           uint32_t width,
                           uint32_t height,
                           VkFormat astc_format,
                           const void* compressed_bytes,
                           size_t byte_size,
                           VkFilter filter = VK_FILTER_NEAREST);

    void destroy();

    VkImage image() const { return image_; }
    VkImageView image_view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    VkFormat format() const { return format_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

// 8x8 (2x2 blocks) VK_FORMAT_ASTC_4x4_UNORM_BLOCK quad-color void-extent test texture (64 bytes).
// Block (0,0) Top-Left: Red (255,0,0,255)
// Block (1,0) Top-Right: Green (0,255,0,255)
// Block (0,1) Bottom-Left: Blue (0,0,255,255)
// Block (1,1) Bottom-Right: Yellow (255,255,0,255)
const uint8_t* get_astc_4x4_8x8_quad_color_bytes(size_t* out_size);

}  // namespace oh2::vk
