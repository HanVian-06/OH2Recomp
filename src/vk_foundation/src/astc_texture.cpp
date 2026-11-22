#include "vk_foundation/astc_texture.h"
#include "vk_foundation/vk_log.h"

#include <cstring>

namespace oh2::vk {

// 8x8 (2x2 ASTC 4x4 blocks = 64 bytes) constant-color void-extent LDR pattern:
// Bits [8:0]=0x1FC, Bit [9]=0 (LDR), Bits [11:10]=3 -> 0xDFC; Bits [63:12]=all 1s (0x1FFF x 4)
// Bytes [0..7] = 0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
// Bytes [8..15] = uint16_t LE R, G, B, A
static const uint8_t kAstc4x4_8x8_QuadColor[64] = {
    // Block (0,0) Top-Left: Red (255, 0, 0, 255)
    0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
    // Block (1,0) Top-Right: Green (0, 255, 0, 255)
    0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF,
    // Block (0,1) Bottom-Left: Blue (0, 0, 255, 255)
    0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    // Block (1,1) Bottom-Right: Yellow (255, 255, 0, 255)
    0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF,
};

const uint8_t* get_astc_4x4_8x8_quad_color_bytes(size_t* out_size) {
    if (out_size) *out_size = sizeof(kAstc4x4_8x8_QuadColor);
    return kAstc4x4_8x8_QuadColor;
}

AstcTexture::~AstcTexture() {
    destroy();
}

bool AstcTexture::is_format_supported(const VkContext& ctx, VkFormat format) {
    switch (format) {
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: return ctx.caps().astc_4x4;
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK: return ctx.caps().astc_6x6;
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK: return ctx.caps().astc_8x8;
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: return ctx.caps().astc_4x4_srgb;
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK: return ctx.caps().astc_6x6_srgb;
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: return ctx.caps().astc_8x8_srgb;
        default: return false;
    }
}

bool AstcTexture::create_and_upload(VkContext& ctx,
                                    StagingRingBuffer& ring,
                                    VkCommandPool cmd_pool,
                                    uint32_t width,
                                    uint32_t height,
                                    VkFormat astc_format,
                                    const void* compressed_bytes,
                                    size_t byte_size,
                                    VkFilter filter) {
    destroy();

    if (!is_format_supported(ctx, astc_format)) {
        M3_LOGE("ASTC format %d not supported natively on device", static_cast<int>(astc_format));
        return false;
    }

    device_ = ctx.device();
    allocator_ = ctx.allocator();
    format_ = astc_format;
    width_ = width;
    height_ = height;

    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = astc_format;
    img_info.extent = {width, height, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkResult res = vmaCreateImage(allocator_, &img_info, &alloc_info,
                                  &image_, &allocation_, nullptr);
    if (res != VK_SUCCESS) {
        M3_LOGE("vmaCreateImage (ASTC %ux%u) failed: %d", width, height, static_cast<int>(res));
        destroy();
        return false;
    }

    StagingAllocation staging{};
    if (!ring.allocate(byte_size, 16, staging)) {
        M3_LOGE("StagingRingBuffer allocate failed for ASTC (%zu B)", byte_size);
        destroy();
        return false;
    }
    std::memcpy(staging.mapped, compressed_bytes, byte_size);
    ring.flush(staging.offset, byte_size);

    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = cmd_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    res = vkAllocateCommandBuffers(device_, &cmd_alloc, &cmd);
    if (res != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence upload_fence = VK_NULL_HANDLE;
    res = vkCreateFence(device_, &fence_info, nullptr, &upload_fence);
    if (res != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, cmd_pool, 1, &cmd);
        destroy();
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image_;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_shader{};
    to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader.image = image_;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_shader);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    res = vkQueueSubmit(ctx.graphics_queue(), 1, &submit_info, upload_fence);
    if (res == VK_SUCCESS) {
        ring.track_submission(staging.end_cursor, upload_fence);
        vkWaitForFences(device_, 1, &upload_fence, VK_TRUE, UINT64_MAX);
        ring.notify_fence_completed(upload_fence);
    }

    vkDestroyFence(device_, upload_fence, nullptr);
    vkFreeCommandBuffers(device_, cmd_pool, 1, &cmd);

    if (res != VK_SUCCESS) {
        M3_LOGE("ASTC upload vkQueueSubmit failed: %d", static_cast<int>(res));
        destroy();
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = astc_format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    res = vkCreateImageView(device_, &view_info, nullptr, &view_);
    if (res != VK_SUCCESS) {
        M3_LOGE("vkCreateImageView (ASTC) failed: %d", static_cast<int>(res));
        destroy();
        return false;
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = filter;
    sampler_info.minFilter = filter;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    res = vkCreateSampler(device_, &sampler_info, nullptr, &sampler_);
    if (res != VK_SUCCESS) {
        M3_LOGE("vkCreateSampler (ASTC) failed: %d", static_cast<int>(res));
        destroy();
        return false;
    }

    return true;
}

void AstcTexture::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
        if (view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view_, nullptr);
            view_ = VK_NULL_HANDLE;
        }
    }
    if (image_ != VK_NULL_HANDLE && allocation_ != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, allocation_);
    }
    image_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
    format_ = VK_FORMAT_UNDEFINED;
    width_ = 0;
    height_ = 0;
}

}  // namespace oh2::vk
