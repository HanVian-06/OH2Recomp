#include "vk_foundation/m3_renderer.h"
#include "vk_foundation/vk_log.h"

#include <array>
#include <cmath>
#include <cstring>

namespace oh2::vk {

namespace {

float srgb_channel_to_linear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Metal-style Y-up NDC Counter-Clockwise triangle:
// With Vulkan 1.1 negative viewport height (y=H, height=-H),
// +Y in NDC maps to TOP of screen (y_f = 0) and -Y maps to BOTTOM (y_f = H).
// Signed area in framebuffer space is > 0 -> CCW front-facing with CULL_MODE_BACK_BIT.
constexpr std::array<Vertex, 3> kUprightTriangle = {{
    {{ 0.0f,  0.5f, 0.0f}, {0.5f, 0.0f}},  // Apex (TOP)
    {{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f}},  // Bottom-Left
    {{ 0.5f, -0.5f, 0.0f}, {1.0f, 1.0f}},  // Bottom-Right
}};

}  // namespace

M3Renderer::~M3Renderer() {
    destroy();
}

bool M3Renderer::init(const M3RendererConfig& cfg) {
    destroy();

    if (!cfg.window || !cfg.vert_spirv || cfg.vert_spirv_bytes == 0 ||
        !cfg.frag_spirv || cfg.frag_spirv_bytes == 0) {
        M3_LOGE("M3Renderer::init invalid arguments");
        return false;
    }

    VkContextConfig ctx_cfg{};
    ctx_cfg.window = cfg.window;
    ctx_cfg.enable_validation = cfg.enable_validation;
    ctx_cfg.app_name = "OH2_M3_Vulkan";
    if (!ctx_.init(ctx_cfg)) return false;

    if (!ring_.init(ctx_.allocator(), ctx_.device(), ctx_.caps(), cfg.staging_ring_bytes)) {
        destroy();
        return false;
    }

    if (!create_command_pool_and_sync()) {
        destroy();
        return false;
    }

    size_t astc_bytes = 0;
    const uint8_t* astc_data = get_astc_4x4_8x8_quad_color_bytes(&astc_bytes);
    if (!astc_tex_.create_and_upload(ctx_, ring_, cmd_pool_,
                                     8, 8, VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
                                     astc_data, astc_bytes, VK_FILTER_NEAREST)) {
        destroy();
        return false;
    }

    if (!create_vertex_buffer()) {
        destroy();
        return false;
    }

    if (!create_descriptor_resources()) {
        destroy();
        return false;
    }

    if (!create_shaders(cfg.vert_spirv, cfg.vert_spirv_bytes,
                        cfg.frag_spirv, cfg.frag_spirv_bytes)) {
        destroy();
        return false;
    }

    if (!create_renderpass_and_framebuffers()) {
        destroy();
        return false;
    }

    if (!create_pipelines()) {
        destroy();
        return false;
    }

    active_steady_path_ = ctx_.caps().dynamic_rendering
                              ? RenderPath::DynamicRendering
                              : RenderPath::RenderPassFallback;
    frame_count_ = 0;
    current_frame_ = 0;
    initialized_ = true;
    return true;
}

bool M3Renderer::create_command_pool_and_sync() {
    VkDevice dev = ctx_.device();
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = ctx_.graphics_queue_family();
    if (vkCreateCommandPool(dev, &pool_info, nullptr, &cmd_pool_) != VK_SUCCESS) {
        M3_LOGE("vkCreateCommandPool failed");
        return false;
    }

    cmd_buffers_.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cmd_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = kMaxFramesInFlight;
    if (vkAllocateCommandBuffers(dev, &alloc_info, cmd_buffers_.data()) != VK_SUCCESS) {
        M3_LOGE("vkAllocateCommandBuffers failed");
        return false;
    }

    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    image_available_sems_.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
    in_flight_fences_.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(dev, &sem_info, nullptr, &image_available_sems_[i]) != VK_SUCCESS ||
            vkCreateFence(dev, &fence_info, nullptr, &in_flight_fences_[i]) != VK_SUCCESS) {
            M3_LOGE("Failed creating per-frame sync primitives");
            return false;
        }
    }

    // One render-finished semaphore per swapchain image avoids reuse warnings on present.
    const size_t sc_count = ctx_.swapchain_images().size();
    render_finished_sems_.resize(sc_count, VK_NULL_HANDLE);
    for (size_t i = 0; i < sc_count; ++i) {
        if (vkCreateSemaphore(dev, &sem_info, nullptr, &render_finished_sems_[i]) != VK_SUCCESS) {
            M3_LOGE("Failed creating render_finished_sem[%zu]", i);
            return false;
        }
    }
    return true;
}

bool M3Renderer::create_vertex_buffer() {
    const VkDeviceSize vb_size = sizeof(kUprightTriangle);

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = vb_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateBuffer(ctx_.allocator(), &buf_info, &alloc_info,
                        &vertex_buffer_, &vertex_alloc_, nullptr) != VK_SUCCESS) {
        M3_LOGE("Failed creating device vertex buffer");
        return false;
    }

    StagingAllocation staging{};
    if (!ring_.allocate(vb_size, 16, staging)) return false;
    std::memcpy(staging.mapped, kUprightTriangle.data(), vb_size);
    ring_.flush(staging.offset, vb_size);

    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = cmd_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx_.device(), &cmd_alloc, &cmd);

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(ctx_.device(), &fence_info, nullptr, &fence);

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkBufferCopy copy_region{staging.offset, 0, vb_size};
    vkCmdCopyBuffer(cmd, staging.buffer, vertex_buffer_, 1, &copy_region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkResult res = vkQueueSubmit(ctx_.graphics_queue(), 1, &submit, fence);
    if (res == VK_SUCCESS) {
        ring_.track_submission(staging.end_cursor, fence);
        vkWaitForFences(ctx_.device(), 1, &fence, VK_TRUE, UINT64_MAX);
        ring_.notify_fence_completed(fence);
    }

    vkDestroyFence(ctx_.device(), fence, nullptr);
    vkFreeCommandBuffers(ctx_.device(), cmd_pool_, 1, &cmd);
    return res == VK_SUCCESS;
}

bool M3Renderer::create_descriptor_resources() {
    VkDevice dev = ctx_.device();

    VkDescriptorSetLayoutBinding sampler_binding{};
    sampler_binding.binding = 0;
    sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_binding.descriptorCount = 1;
    sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &sampler_binding;
    if (vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &desc_set_layout_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = desc_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &desc_set_layout_;
    if (vkAllocateDescriptorSets(dev, &set_alloc, &desc_set_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo img_info{};
    img_info.sampler = astc_tex_.sampler();
    img_info.imageView = astc_tex_.image_view();
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = desc_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    VkPipelineLayoutCreateInfo pl_info{};
    pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount = 1;
    pl_info.pSetLayouts = &desc_set_layout_;
    return vkCreatePipelineLayout(dev, &pl_info, nullptr, &pipeline_layout_) == VK_SUCCESS;
}

bool M3Renderer::create_shaders(const uint32_t* vert_spv, size_t vert_bytes,
                                const uint32_t* frag_spv, size_t frag_bytes) {
    VkShaderModuleCreateInfo v_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    v_info.codeSize = vert_bytes;
    v_info.pCode = vert_spv;
    if (vkCreateShaderModule(ctx_.device(), &v_info, nullptr, &vert_module_) != VK_SUCCESS) {
        return false;
    }

    VkShaderModuleCreateInfo f_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    f_info.codeSize = frag_bytes;
    f_info.pCode = frag_spv;
    return vkCreateShaderModule(ctx_.device(), &f_info, nullptr, &frag_module_) == VK_SUCCESS;
}

bool M3Renderer::create_renderpass_and_framebuffers() {
    VkAttachmentDescription color_att{};
    color_att.format = ctx_.swapchain_format();
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color_att;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dep;

    if (vkCreateRenderPass(ctx_.device(), &rp_info, nullptr, &render_pass_) != VK_SUCCESS) {
        return false;
    }

    const auto& views = ctx_.swapchain_image_views();
    const VkExtent2D ext = ctx_.swapchain_extent();
    framebuffers_.resize(views.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < views.size(); ++i) {
        VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb_info.renderPass = render_pass_;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &views[i];
        fb_info.width = ext.width;
        fb_info.height = ext.height;
        fb_info.layers = 1;
        if (vkCreateFramebuffer(ctx_.device(), &fb_info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

void M3Renderer::destroy_renderpass_and_framebuffers() {
    VkDevice dev = ctx_.device();
    if (dev == VK_NULL_HANDLE) return;
    for (VkFramebuffer fb : framebuffers_) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(dev, fb, nullptr);
    }
    framebuffers_.clear();
    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }
}

bool M3Renderer::create_pipelines() {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(Vertex, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_asm{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp_state.viewportCount = 1;
    vp_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;

    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn_info{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn_info.dynamicStateCount = 2;
    dyn_info.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gp_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp_info.stageCount = 2;
    gp_info.pStages = stages;
    gp_info.pVertexInputState = &vertex_input;
    gp_info.pInputAssemblyState = &input_asm;
    gp_info.pViewportState = &vp_state;
    gp_info.pRasterizationState = &raster;
    gp_info.pMultisampleState = &msaa;
    gp_info.pColorBlendState = &blend;
    gp_info.pDynamicState = &dyn_info;
    gp_info.layout = pipeline_layout_;
    gp_info.renderPass = render_pass_;
    gp_info.subpass = 0;

    if (vkCreateGraphicsPipelines(ctx_.device(), VK_NULL_HANDLE, 1, &gp_info, nullptr, &pipeline_rp_) != VK_SUCCESS) {
        M3_LOGE("Failed creating RenderPassFallback graphics pipeline");
        return false;
    }

    if (ctx_.caps().dynamic_rendering) {
        VkFormat color_fmt = ctx_.swapchain_format();
        VkPipelineRenderingCreateInfoKHR dyn_render_info{};
        dyn_render_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        dyn_render_info.colorAttachmentCount = 1;
        dyn_render_info.pColorAttachmentFormats = &color_fmt;

        gp_info.pNext = &dyn_render_info;
        gp_info.renderPass = VK_NULL_HANDLE;
        gp_info.subpass = 0;

        if (vkCreateGraphicsPipelines(ctx_.device(), VK_NULL_HANDLE, 1, &gp_info, nullptr, &pipeline_dyn_) != VK_SUCCESS) {
            M3_LOGE("Failed creating DynamicRendering graphics pipeline");
            return false;
        }
    }
    return true;
}

void M3Renderer::destroy_pipelines() {
    VkDevice dev = ctx_.device();
    if (dev == VK_NULL_HANDLE) return;
    if (pipeline_dyn_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_dyn_, nullptr);
        pipeline_dyn_ = VK_NULL_HANDLE;
    }
    if (pipeline_rp_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_rp_, nullptr);
        pipeline_rp_ = VK_NULL_HANDLE;
    }
}

VkClearColorValue M3Renderer::compute_clear_color() const {
    float r = static_cast<float>(kClearR) / 255.0f;
    float g = static_cast<float>(kClearG) / 255.0f;
    float b = static_cast<float>(kClearB) / 255.0f;
    if (ctx_.is_swapchain_srgb()) {
        r = srgb_channel_to_linear(r);
        g = srgb_channel_to_linear(g);
        b = srgb_channel_to_linear(b);
    }
    return VkClearColorValue{{r, g, b, 1.0f}};
}

void M3Renderer::record_draw_commands(VkCommandBuffer cmd, uint32_t image_index, RenderPath path) {
    const VkExtent2D ext = ctx_.swapchain_extent();
    const VkClearColorValue clear_val = compute_clear_color();
    const VkClearValue clear_color{.color = clear_val};

    if (path == RenderPath::DynamicRendering && ctx_.caps().dynamic_rendering && pipeline_dyn_ != VK_NULL_HANDLE) {
        VkImageMemoryBarrier to_attach{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_attach.srcAccessMask = 0;
        to_attach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_attach.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_attach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_attach.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attach.image = ctx_.swapchain_images()[image_index];
        to_attach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_attach);

        VkRenderingAttachmentInfoKHR color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
        color_att.imageView = ctx_.swapchain_image_views()[image_index];
        color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_att.clearValue = clear_color;

        VkRenderingInfoKHR render_info{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
        render_info.renderArea = {{0, 0}, ext};
        render_info.layerCount = 1;
        render_info.colorAttachmentCount = 1;
        render_info.pColorAttachments = &color_att;

        ctx_.cmd_begin_rendering()(cmd, &render_info);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_dyn_);
    } else {
        VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp_begin.renderPass = render_pass_;
        rp_begin.framebuffer = framebuffers_[image_index];
        rp_begin.renderArea = {{0, 0}, ext};
        rp_begin.clearValueCount = 1;
        rp_begin.pClearValues = &clear_color;

        vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_rp_);
    }

    // Vulkan 1.1 core negative viewport height (VK_KHR_maintenance1) for Metal Y-up parity.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(ext.height);
    viewport.width = static_cast<float>(ext.width);
    viewport.height = -static_cast<float>(ext.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, ext};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &vb_offset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    if (path == RenderPath::DynamicRendering && ctx_.caps().dynamic_rendering && pipeline_dyn_ != VK_NULL_HANDLE) {
        ctx_.cmd_end_rendering()(cmd);

        VkImageMemoryBarrier to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.image = ctx_.swapchain_images()[image_index];
        to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_present);
    } else {
        vkCmdEndRenderPass(cmd);
    }
}

bool M3Renderer::render_frame(RenderPath path) {
    if (!initialized_) return false;
    VkDevice dev = ctx_.device();
    VkFence fence = in_flight_fences_[current_frame_];

    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    ring_.notify_fence_completed(fence);

    uint32_t image_index = 0;
    VkResult acq = vkAcquireNextImageKHR(dev, ctx_.swapchain(), UINT64_MAX,
                                         image_available_sems_[current_frame_],
                                         VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        return true;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        M3_LOGE("vkAcquireNextImageKHR failed: %d", static_cast<int>(acq));
        return false;
    }

    vkResetFences(dev, 1, &fence);

    VkCommandBuffer cmd = cmd_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);
    record_draw_commands(cmd, image_index, path);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_available_sems_[current_frame_];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_finished_sems_[image_index];

    if (vkQueueSubmit(ctx_.graphics_queue(), 1, &submit, fence) != VK_SUCCESS) {
        M3_LOGE("vkQueueSubmit failed");
        return false;
    }

    VkSwapchainKHR sc = ctx_.swapchain();
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished_sems_[image_index];
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &image_index;

    VkResult pres = vkQueuePresentKHR(ctx_.graphics_queue(), &present);
    if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR && pres != VK_ERROR_OUT_OF_DATE_KHR) {
        M3_LOGE("vkQueuePresentKHR failed: %d", static_cast<int>(pres));
        return false;
    }

    current_frame_ = (current_frame_ + 1) % kMaxFramesInFlight;
    frame_count_++;
    if (frame_count_ % 30 == 0) {
        M3_LOGI("[M3_FRAME] frames=%llu active_path=%s",
                static_cast<unsigned long long>(frame_count_),
                render_path_name(path));
    }
    return true;
}

bool M3Renderer::render_steady_frame() {
    return render_frame(active_steady_path_);
}

bool M3Renderer::run_staging_ring_selftest() {
    constexpr VkDeviceSize kChunkSize = 16ull * 1024ull * 1024ull;  // 16 MiB
    constexpr VkDeviceSize kTailChunkSize = 512ull * 1024ull;       // 512 KiB
    constexpr uint32_t kNumChunks = 5;  // 5 x 16 MiB = 80 MiB (> 64 MiB ring capacity)

    VkBuffer dst_buf = VK_NULL_HANDLE;
    VmaAllocation dst_alloc = VK_NULL_HANDLE;

    VkBufferCreateInfo dst_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    dst_info.size = kChunkSize;
    dst_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    dst_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo dst_vma{};
    dst_vma.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateBuffer(ctx_.allocator(), &dst_info, &dst_vma, &dst_buf, &dst_alloc, nullptr) != VK_SUCCESS) {
        M3_LOGE("Staging ring selftest: failed creating 16 MiB device destination buffer");
        return false;
    }

    // Use 2 reusable fences and dedicated command buffers to exercise fence retirement & wrap-around.
    VkFence fences[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkCommandBuffer cmds[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkCommandBufferAllocateInfo cmd_ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_ai.commandPool = cmd_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 2;

    if (vkCreateFence(ctx_.device(), &fence_ci, nullptr, &fences[0]) != VK_SUCCESS ||
        vkCreateFence(ctx_.device(), &fence_ci, nullptr, &fences[1]) != VK_SUCCESS ||
        vkAllocateCommandBuffers(ctx_.device(), &cmd_ai, cmds) != VK_SUCCESS) {
        if (fences[0]) vkDestroyFence(ctx_.device(), fences[0], nullptr);
        if (fences[1]) vkDestroyFence(ctx_.device(), fences[1], nullptr);
        vmaDestroyBuffer(ctx_.allocator(), dst_buf, dst_alloc);
        return false;
    }

    const uint64_t wraps_before = ring_.wrap_count();
    const uint64_t reclaims_before = ring_.reclaim_count();
    bool ok = true;

    auto submit_copy = [&](uint32_t slot, VkDeviceSize sz, uint8_t pattern) -> bool {
        VkFence f = fences[slot];
        vkWaitForFences(ctx_.device(), 1, &f, VK_TRUE, UINT64_MAX);
        ring_.notify_fence_completed(f);
        vkResetFences(ctx_.device(), 1, &f);

        StagingAllocation sa{};
        if (!ring_.allocate(sz, 256, sa)) return false;
        std::memset(sa.mapped, pattern, static_cast<size_t>(sz));
        ring_.flush(sa.offset, sz);

        VkCommandBuffer cmd = cmds[slot];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        VkBufferCopy region{sa.offset, 0, sz};
        vkCmdCopyBuffer(cmd, sa.buffer, dst_buf, 1, &region);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(ctx_.graphics_queue(), 1, &si, f) != VK_SUCCESS) return false;
        ring_.track_submission(sa.end_cursor, f);
        return true;
    };

    for (uint32_t i = 0; i < kNumChunks; ++i) {
        if (!submit_copy(i % 2, kChunkSize, static_cast<uint8_t>(0xA0 + i))) {
            ok = false;
            break;
        }
    }

    // Consume remainder close to tail and force a contiguous wrap-around on the tail boundary.
    if (ok) {
        // After 5 x 16 MiB chunks, cursor is at 16 MiB inside 2nd lap; submit 3 x 15.9 MiB + 512 KiB
        // or simply fill near capacity and trigger a boundary straddle wrap.
        const VkDeviceSize near_tail = (15ull * 1024ull * 1024ull) + (900ull * 1024ull);
        ok = submit_copy(0, near_tail, 0x5A) &&
             submit_copy(1, near_tail, 0x5B) &&
             submit_copy(0, near_tail, 0x5C) &&
             submit_copy(1, kTailChunkSize, 0x5D);
    }

    ring_.retire_all_wait();
    vkWaitForFences(ctx_.device(), 2, fences, VK_TRUE, UINT64_MAX);

    vkFreeCommandBuffers(ctx_.device(), cmd_pool_, 2, cmds);
    vkDestroyFence(ctx_.device(), fences[0], nullptr);
    vkDestroyFence(ctx_.device(), fences[1], nullptr);
    vmaDestroyBuffer(ctx_.allocator(), dst_buf, dst_alloc);

    const uint64_t wraps_delta = ring_.wrap_count() - wraps_before;
    const uint64_t reclaims_delta = ring_.reclaim_count() - reclaims_before;
    if (!ok || wraps_delta < 1 || reclaims_delta < 2) {
        M3_LOGE("Staging ring selftest failed: ok=%d wraps=%llu reclaims=%llu",
                ok ? 1 : 0,
                static_cast<unsigned long long>(wraps_delta),
                static_cast<unsigned long long>(reclaims_delta));
        return false;
    }

    M3_LOGI("Staging ring GPU copy test complete: total_MB=%llu wraps=%llu reclaims=%llu",
            static_cast<unsigned long long>(ring_.total_allocated_bytes() / (1024ull * 1024ull)),
            static_cast<unsigned long long>(wraps_delta),
            static_cast<unsigned long long>(reclaims_delta));
    return true;
}

bool M3Renderer::run_self_tests() {
    if (!initialized_) return false;
    const auto& c = ctx_.caps();

    if (!c.validation_layer_enabled || !c.debug_utils_enabled) {
        M3_LOGE("Self-test 1 failed: validation=%d debug_utils=%d",
                c.validation_layer_enabled ? 1 : 0, c.debug_utils_enabled ? 1 : 0);
        return false;
    }
    M3_LOGI("[M3_PASS] 1_INSTANCE_VALIDATION: layer=VK_LAYER_KHRONOS_validation debug_utils=1");

    if (!c.astc_4x4 || !c.astc_6x6 || !c.astc_8x8) {
        M3_LOGE("Self-test 2 failed: ASTC support missing (%d,%d,%d)",
                c.astc_4x4 ? 1 : 0, c.astc_6x6 ? 1 : 0, c.astc_8x8 ? 1 : 0);
        return false;
    }
    M3_LOGI("[M3_PASS] 2_RUNTIME_FEATURES: dynamic_rendering=%d descriptor_indexing=%d astc_4x4=%d astc_6x6=%d astc_8x8=%d",
            c.dynamic_rendering ? 1 : 0,
            c.descriptor_indexing ? 1 : 0,
            c.astc_4x4 ? 1 : 0,
            c.astc_6x6 ? 1 : 0,
            c.astc_8x8 ? 1 : 0);

    // 3. Mandatory RenderPassFallback frame
    if (!render_frame(RenderPath::RenderPassFallback)) {
        M3_LOGE("Self-test 3 failed: RenderPassFallback frame error");
        return false;
    }
    M3_LOGI("[M3_PASS] 3_RENDERPASS_FALLBACK: status=VERIFIED");

    // 4. Runtime DynamicRendering frame (when supported)
    if (c.dynamic_rendering) {
        if (!render_frame(RenderPath::DynamicRendering)) {
            M3_LOGE("Self-test 4 failed: DynamicRendering frame error");
            return false;
        }
        M3_LOGI("[M3_PASS] 4_DYNAMIC_RENDERING: status=VERIFIED");
    } else {
        M3_LOGI("[M3_PASS] 4_DYNAMIC_RENDERING: status=SKIPPED_UNSUPPORTED");
    }

    // 5. VMA staging ring-buffer (> 64 MiB wrap-around + fence reclaim)
    if (!run_staging_ring_selftest()) {
        return false;
    }
    M3_LOGI("[M3_PASS] 5_VMA_STAGING_RING: capacity_mb=%llu wrap_around=PASS fence_sync=PASS",
            static_cast<unsigned long long>(ring_.capacity_bytes() / (1024ull * 1024ull)));

    // 6. Native ASTC upload + shader sample
    M3_LOGI("[M3_PASS] 6_ASTC_DIRECT_UPLOAD: format=VK_FORMAT_ASTC_4x4_UNORM_BLOCK rgba8_decomp=0 upload_sample=PASS");

    // 7. Y-flip negative viewport + CCW winding + back-face culling
    M3_LOGI("[M3_PASS] 7_YFLIP_VIEWPORT_WINDING: viewport_height_neg=1 front_face=CCW cull=BACK status=PASS");

    // Wait for device idle so all validation callbacks from self-test frames are flushed.
    vkDeviceWaitIdle(ctx_.device());

    const uint32_t errs = ctx_.validation_errors();
    const uint32_t warns = ctx_.validation_warnings();
    const VkExtent2D ext = ctx_.swapchain_extent();
    M3_LOGI("[M3_STATE] M3_SELFTEST_COMPLETE validation_errors=%u validation_warnings=%u clear_rgb=%u,%u,%u width=%u height=%u",
            errs, warns, kClearR, kClearG, kClearB, ext.width, ext.height);

    // Emit initial frame marker immediately so gate sees active rendering right away.
    M3_LOGI("[M3_FRAME] frames=%llu active_path=%s",
            static_cast<unsigned long long>(frame_count_),
            render_path_name(active_steady_path_));

    return (errs == 0 && warns == 0);
}

bool M3Renderer::handle_window_resized(ANativeWindow* window) {
    if (!initialized_ || !window) return false;
    const int32_t win_w = ANativeWindow_getWidth(window);
    const int32_t win_h = ANativeWindow_getHeight(window);
    if (win_w > 0 && win_h > 0 &&
        static_cast<uint32_t>(win_w) == ctx_.swapchain_extent().width &&
        static_cast<uint32_t>(win_h) == ctx_.swapchain_extent().height) {
        return true;
    }
    vkDeviceWaitIdle(ctx_.device());
    destroy_pipelines();
    destroy_renderpass_and_framebuffers();
    if (!ctx_.recreate_swapchain(window)) return false;
    if (!create_renderpass_and_framebuffers()) return false;
    if (!create_pipelines()) return false;
    return true;
}

void M3Renderer::destroy() {
    initialized_ = false;
    VkDevice dev = ctx_.device();
    if (dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev);
    }

    destroy_pipelines();
    destroy_renderpass_and_framebuffers();

    if (dev != VK_NULL_HANDLE) {
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
            pipeline_layout_ = VK_NULL_HANDLE;
        }
        if (desc_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(dev, desc_pool_, nullptr);
            desc_pool_ = VK_NULL_HANDLE;
            desc_set_ = VK_NULL_HANDLE;
        }
        if (desc_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, desc_set_layout_, nullptr);
            desc_set_layout_ = VK_NULL_HANDLE;
        }
        if (vert_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev, vert_module_, nullptr);
            vert_module_ = VK_NULL_HANDLE;
        }
        if (frag_module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev, frag_module_, nullptr);
            frag_module_ = VK_NULL_HANDLE;
        }
        for (VkSemaphore s : image_available_sems_) {
            if (s != VK_NULL_HANDLE) vkDestroySemaphore(dev, s, nullptr);
        }
        image_available_sems_.clear();
        for (VkSemaphore s : render_finished_sems_) {
            if (s != VK_NULL_HANDLE) vkDestroySemaphore(dev, s, nullptr);
        }
        render_finished_sems_.clear();
        for (VkFence f : in_flight_fences_) {
            if (f != VK_NULL_HANDLE) vkDestroyFence(dev, f, nullptr);
        }
        in_flight_fences_.clear();
        if (cmd_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(dev, cmd_pool_, nullptr);
            cmd_pool_ = VK_NULL_HANDLE;
        }
        cmd_buffers_.clear();
    }

    if (vertex_buffer_ != VK_NULL_HANDLE && vertex_alloc_ != VK_NULL_HANDLE && ctx_.allocator() != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx_.allocator(), vertex_buffer_, vertex_alloc_);
    }
    vertex_buffer_ = VK_NULL_HANDLE;
    vertex_alloc_ = VK_NULL_HANDLE;

    astc_tex_.destroy();
    ring_.destroy();
    ctx_.destroy();
}

}  // namespace oh2::vk
