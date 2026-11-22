#pragma once

#include "vk_foundation/astc_texture.h"
#include "vk_foundation/staging_ring.h"
#include "vk_foundation/vk_context.h"

#include <cstdint>
#include <vector>

namespace oh2::vk {

struct M3RendererConfig {
    ANativeWindow* window = nullptr;
    const uint32_t* vert_spirv = nullptr;
    size_t vert_spirv_bytes = 0;
    const uint32_t* frag_spirv = nullptr;
    size_t frag_spirv_bytes = 0;
    uint64_t staging_ring_bytes = StagingRingBuffer::kMinCapacityBytes;
    bool enable_validation = true;
};

class M3Renderer {
public:
    static constexpr uint32_t kMaxFramesInFlight = 2;
    static constexpr uint8_t kClearR = 16;
    static constexpr uint8_t kClearG = 48;
    static constexpr uint8_t kClearB = 80;

    M3Renderer() = default;
    ~M3Renderer();

    M3Renderer(const M3Renderer&) = delete;
    M3Renderer& operator=(const M3Renderer&) = delete;

    bool init(const M3RendererConfig& cfg);
    bool run_self_tests();
    bool render_frame(RenderPath path);
    bool render_steady_frame();
    bool handle_window_resized(ANativeWindow* window);
    void destroy();

    RenderPath active_steady_path() const { return active_steady_path_; }
    uint64_t frame_count() const { return frame_count_; }
    const VkContext& context() const { return ctx_; }

private:
    bool create_command_pool_and_sync();
    bool create_descriptor_resources();
    bool create_shaders(const uint32_t* vert_spv, size_t vert_bytes,
                        const uint32_t* frag_spv, size_t frag_bytes);
    bool create_vertex_buffer();
    bool create_renderpass_and_framebuffers();
    void destroy_renderpass_and_framebuffers();
    bool create_pipelines();
    void destroy_pipelines();
    bool run_staging_ring_selftest();

    VkClearColorValue compute_clear_color() const;
    void record_draw_commands(VkCommandBuffer cmd, uint32_t image_index, RenderPath path);

    VkContext ctx_;
    StagingRingBuffer ring_;
    AstcTexture astc_tex_;

    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmd_buffers_;
    std::vector<VkSemaphore> image_available_sems_;
    std::vector<VkSemaphore> render_finished_sems_;
    std::vector<VkFence> in_flight_fences_;

    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_module_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkPipeline pipeline_rp_ = VK_NULL_HANDLE;
    VkPipeline pipeline_dyn_ = VK_NULL_HANDLE;

    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VmaAllocation vertex_alloc_ = VK_NULL_HANDLE;

    uint32_t current_frame_ = 0;
    uint64_t frame_count_ = 0;
    RenderPath active_steady_path_ = RenderPath::RenderPassFallback;
    bool initialized_ = false;
};

}  // namespace oh2::vk
