#include "mtl_vk_internal.h"
#include "../libc_wrapper/libc_wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" void oh2_objc_msgSend(void);

namespace oh2::mtlvk {

namespace {
struct BlockLayout1 {
    void *isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke)(void *, void *);
};

static std::string nsstr_to_std(const void *obj) {
    if (!obj) return "";
    char buf[512];
    const char *s = darwin_cfstring_utf8(obj, buf, sizeof(buf));
    if (!s || std::strcmp(s, "(null)") == 0 || std::strcmp(s, "<Object>") == 0) return "";
    return s;
}
} // namespace

MtlVkState &get_state() {
    static MtlVkState s_state;
    return s_state;
}

void *alloc_mtl_wrap(const char *cls_name, void *impl_ptr) {
    void *cls = oh2_objc_ensure_class(cls_name, "NSObject");
    auto *w = static_cast<MtlObjWrap<void> *>(oh2_objc_alloc_instance(cls, sizeof(MtlObjWrap<void>)));
    if (w) {
        w->impl = impl_ptr;
    }
    return w;
}

void *alloc_mtl_wrap_managed(const char *cls_name, void *impl_ptr, void (*dealloc_fn)(void *), bool autorelease_obj) {
    void *w = alloc_mtl_wrap(cls_name, impl_ptr);
    if (w) {
        if (dealloc_fn) {
            oh2_objc_set_dealloc(w, dealloc_fn);
        } else {
            oh2_objc_mark_reclaimable(w);
        }
        if (autorelease_obj) {
            typedef void *(*auto_fn_t)(void *, const char *);
            ((auto_fn_t)oh2_objc_msgSend)(w, oh2_objc_intern_selector("autorelease"));
        }
    }
    return w;
}

VkDescriptorPool create_frame_descriptor_pool(VkDevice dev) {
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32768},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  32768},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  4096},
        {VK_DESCRIPTOR_TYPE_SAMPLER,        16384},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 16384;
    dpci.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
    dpci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &pool) != VK_SUCCESS) {
        MTLVK_LOGE("vkCreateDescriptorPool failed");
        return VK_NULL_HANDLE;
    }
    return pool;
}

void enqueue_deferred_buffer(VkBuffer buf, VmaAllocation alloc) {
    if (buf == VK_NULL_HANDLE && alloc == VK_NULL_HANDLE) return;
    MtlVkState &st = get_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    DeferredDestroy d{};
    d.kind = DeferredDestroy::BUFFER;
    d.buffer = buf;
    d.allocation = alloc;
    d.frames_remaining = kMaxFramesInFlight + 1;
    st.deferred_destroys.push_back(d);
}

void enqueue_deferred_image(VkImage img, VmaAllocation alloc, VkImageView view) {
    if (img == VK_NULL_HANDLE && alloc == VK_NULL_HANDLE && view == VK_NULL_HANDLE) return;
    MtlVkState &st = get_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    DeferredDestroy d{};
    d.kind = DeferredDestroy::IMAGE;
    d.image = img;
    d.allocation = alloc;
    d.image_view = view;
    d.frames_remaining = kMaxFramesInFlight + 1;
    st.deferred_destroys.push_back(d);
}

void enqueue_deferred_view(VkImageView view) {
    if (view == VK_NULL_HANDLE) return;
    MtlVkState &st = get_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    DeferredDestroy d{};
    d.kind = DeferredDestroy::IMAGE_VIEW;
    d.image_view = view;
    d.frames_remaining = kMaxFramesInFlight + 1;
    st.deferred_destroys.push_back(d);
}

void enqueue_deferred_framebuffer(VkFramebuffer fb) {
    if (fb == VK_NULL_HANDLE) return;
    MtlVkState &st = get_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    DeferredDestroy d{};
    d.kind = DeferredDestroy::FRAMEBUFFER;
    d.framebuffer = fb;
    d.frames_remaining = kMaxFramesInFlight + 1;
    st.deferred_destroys.push_back(d);
}

void enqueue_deferred_sampler(VkSampler smp) {
    if (smp == VK_NULL_HANDLE) return;
    MtlVkState &st = get_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    DeferredDestroy d{};
    d.kind = DeferredDestroy::SAMPLER;
    d.sampler = smp;
    d.frames_remaining = kMaxFramesInFlight + 1;
    st.deferred_destroys.push_back(d);
}

void tick_deferred_destroys(bool force_all) {
    MtlVkState &st = get_state();
    if (!st.initialized) return;
    std::vector<DeferredDestroy> ready;
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        if (st.deferred_destroys.empty()) return;
        std::vector<DeferredDestroy> keep;
        keep.reserve(st.deferred_destroys.size());
        for (DeferredDestroy &d : st.deferred_destroys) {
            if (force_all || d.frames_remaining <= 1) {
                ready.push_back(d);
            } else {
                d.frames_remaining--;
                keep.push_back(d);
            }
        }
        st.deferred_destroys.swap(keep);
    }

    VkDevice dev = st.vk_ctx.device();
    VmaAllocator alloc = st.vk_ctx.allocator();
    for (const DeferredDestroy &d : ready) {
        switch (d.kind) {
            case DeferredDestroy::BUFFER:
                if (d.buffer != VK_NULL_HANDLE || d.allocation != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(alloc, d.buffer, d.allocation);
                }
                break;
            case DeferredDestroy::IMAGE:
                if (d.image_view != VK_NULL_HANDLE) vkDestroyImageView(dev, d.image_view, nullptr);
                if (d.image != VK_NULL_HANDLE || d.allocation != VK_NULL_HANDLE) {
                    vmaDestroyImage(alloc, d.image, d.allocation);
                }
                break;
            case DeferredDestroy::IMAGE_VIEW:
                if (d.image_view != VK_NULL_HANDLE) vkDestroyImageView(dev, d.image_view, nullptr);
                break;
            case DeferredDestroy::FRAMEBUFFER:
                if (d.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(dev, d.framebuffer, nullptr);
                break;
            case DeferredDestroy::SAMPLER:
                if (d.sampler != VK_NULL_HANDLE) vkDestroySampler(dev, d.sampler, nullptr);
                break;
        }
    }
}

static void *copy_objc_block(void *blk) {
    if (!blk) return nullptr;
    typedef void *(*bcopy_t)(const void *);
    static bcopy_t fn = (bcopy_t)darwin_resolve_symbol(nullptr, "__Block_copy");
    return fn ? fn(blk) : blk;
}

static void release_objc_block(void *blk) {
    if (!blk) return;
    typedef void (*brel_t)(const void *);
    static brel_t fn = (brel_t)darwin_resolve_symbol(nullptr, "__Block_release");
    if (fn) fn(blk);
}

void invoke_objc_block_1(void *block, void *arg1) {
    if (!block) return;
    auto *b = static_cast<BlockLayout1 *>(block);
    if (b->invoke) {
        b->invoke(block, arg1);
    }
}

void *create_ns_error(const char *msg) {
    void *cls = oh2_objc_get_class("NSError");
    if (!cls) cls = oh2_objc_ensure_class("NSError", "NSObject");
    void *err = oh2_objc_alloc_instance(cls, 64);
    (void)msg;
    return err;
}

void transition_image_layout(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                             uint32_t base_mip, uint32_t mip_count,
                             uint32_t base_layer, uint32_t layer_count,
                             VkImageLayout old_layout, VkImageLayout new_layout) {
    if (old_layout == new_layout || image == VK_NULL_HANDLE) return;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = base_mip;
    barrier.subresourceRange.levelCount = mip_count;
    barrier.subresourceRange.baseArrayLayer = base_layer;
    barrier.subresourceRange.layerCount = layer_count;

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    switch (old_layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            barrier.srcAccessMask = 0;
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_GENERAL:
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            barrier.srcAccessMask = 0;
            src_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            break;
        default:
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            break;
    }

    switch (new_layout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_GENERAL:
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            barrier.dstAccessMask = 0;
            dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            break;
        default:
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            break;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

bool execute_immediate_upload(void (*record_fn)(VkCommandBuffer, void *), void *user_data) {
    MtlVkState &st = get_state();
    if (!st.initialized || !record_fn) return false;
    VkDevice dev = st.vk_ctx.device();

    VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = st.upload_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &alloc_info, &cmd) != VK_SUCCESS) return false;

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    record_fn(cmd, user_data);

    vkEndCommandBuffer(cmd);

    vkResetFences(dev, 1, &st.upload_fence);
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    VkResult res = vkQueueSubmit(st.vk_ctx.graphics_queue(), 1, &sub, st.upload_fence);
    if (res == VK_SUCCESS) {
        vkWaitForFences(dev, 1, &st.upload_fence, VK_TRUE, UINT64_MAX);
        st.staging.notify_fence_completed(st.upload_fence);
    }
    vkFreeCommandBuffers(dev, st.upload_pool, 1, &cmd);
    return res == VK_SUCCESS;
}

VkShaderModule create_shader_module_from_words(const uint32_t *words, size_t byte_size) {
    MtlVkState &st = get_state();
    if (!st.initialized || !words || byte_size < 16 || (byte_size & 3u) != 0) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = byte_size;
    info.pCode = words;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(st.vk_ctx.device(), &info, nullptr, &mod) != VK_SUCCESS) {
        MTLVK_LOGE("vkCreateShaderModule failed (size=%zu)", byte_size);
        return VK_NULL_HANDLE;
    }
    return mod;
}

VkShaderModule get_or_create_shader_module(const SpirvShaderEntry *entry) {
    if (!entry || !entry->spirv_words || entry->spirv_size_bytes == 0) return VK_NULL_HANDLE;
    MtlVkState &st = get_state();
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.shader_modules.find(entry->shader_id);
        if (it != st.shader_modules.end()) return it->second;
    }
    VkShaderModule mod = create_shader_module_from_words(entry->spirv_words, entry->spirv_size_bytes);
    if (mod != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lk(st.mutex);
        auto [it, inserted] = st.shader_modules.emplace(entry->shader_id, mod);
        if (!inserted) {
            vkDestroyShaderModule(st.vk_ctx.device(), mod, nullptr);
            mod = it->second;
        }
    }
    return mod;
}

VkCommandBuffer mtl_begin_command_buffer_internal(MtlCommandBufferImpl *cb) {
    MtlVkState &st = get_state();
    if (!st.initialized || !cb) return VK_NULL_HANDLE;
    if (cb->recording && cb->cmd != VK_NULL_HANDLE) return cb->cmd;

    VkDevice dev = st.vk_ctx.device();
    uint32_t fidx = st.next_frame_idx;
    st.next_frame_idx = (st.next_frame_idx + 1) % kMaxFramesInFlight;
    FrameSyncSlot &slot = st.frames[fidx];

    if (slot.submitted) {
        vkWaitForFences(dev, 1, &slot.in_flight_fence, VK_TRUE, UINT64_MAX);
        st.staging.notify_fence_completed(slot.in_flight_fence);
        slot.submitted = false;
        tick_deferred_destroys(false);
    }
    vkResetFences(dev, 1, &slot.in_flight_fence);
    vkResetCommandPool(dev, slot.cmd_pool, 0);
    vkResetDescriptorPool(dev, slot.desc_pool, 0);
    for (VkDescriptorPool p : slot.overflow_desc_pools) {
        if (p != VK_NULL_HANDLE) vkResetDescriptorPool(dev, p, 0);
    }
    slot.active_desc_pool_idx = 0;
    slot.inline_cursor = 0;

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.cmd, &begin_info) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    cb->frame_idx = fidx;
    cb->cmd = slot.cmd;
    cb->recording = true;
    cb->committed = false;
    cb->status = 1;
    return cb->cmd;
}

bool mtl_alloc_inline_bytes(MtlCommandBufferImpl *cb, const void *data, uint64_t len, VkBuffer *out_buf, uint64_t *out_off) {
    MtlVkState &st = get_state();
    if (!cb || !cb->recording || !out_buf || !out_off) return false;
    FrameSyncSlot &slot = st.frames[cb->frame_idx];
    if (!slot.inline_mapped || slot.inline_buf == VK_NULL_HANDLE) return false;
    uint64_t aligned_off = (slot.inline_cursor + 255ULL) & ~255ULL;
    uint64_t copy_len = std::max<uint64_t>(len, 16ULL);
    if (aligned_off + copy_len > slot.inline_capacity) {
        static std::atomic<uint32_t> inline_ovf{0};
        uint32_t c = inline_ovf.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c <= 10 || (c % 1000) == 0) {
            MTLVK_LOGE("inline_buf overflow #%u (cursor=%llu need=%llu cap=%llu)",
                       c, static_cast<unsigned long long>(slot.inline_cursor),
                       static_cast<unsigned long long>(copy_len),
                       static_cast<unsigned long long>(slot.inline_capacity));
        }
        return false;
    }
    if (data && len > 0) {
        std::memcpy(slot.inline_mapped + aligned_off, data, (size_t)len);
        vmaFlushAllocation(st.vk_ctx.allocator(), slot.inline_alloc, aligned_off, len);
    }
    slot.inline_cursor = aligned_off + copy_len;
    *out_buf = slot.inline_buf;
    *out_off = aligned_off;
    return true;
}

bool mtl_acquire_swapchain_image_internal(MtlMetalLayerImpl *layer, uint32_t *out_sc_idx, VkSemaphore *out_wait_sem) {
    MtlVkState &st = get_state();
    if (!st.initialized || !out_sc_idx || !out_wait_sem) return false;

    VkDevice dev = st.vk_ctx.device();
    /* Drain oldest in-flight frame first so recycled acquire semaphore is guaranteed idle on GPU */
    uint32_t fidx = st.next_frame_idx;
    if (st.frames[fidx].submitted) {
        vkWaitForFences(dev, 1, &st.frames[fidx].in_flight_fence, VK_TRUE, UINT64_MAX);
        st.staging.notify_fence_completed(st.frames[fidx].in_flight_fence);
        st.frames[fidx].submitted = false;
        tick_deferred_destroys(false);
    }

    VkSemaphore acq_sem = st.acquire_sem_pool[st.next_acquire_sem];
    st.next_acquire_sem = (st.next_acquire_sem + 1) % 8;

    uint32_t sc_idx = 0;
    VkResult res = vkAcquireNextImageKHR(
        dev, st.vk_ctx.swapchain(), UINT64_MAX,
        acq_sem, VK_NULL_HANDLE, &sc_idx);

    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        st.swapchain_out_of_date = true;
        return false;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        MTLVK_LOGE("vkAcquireNextImageKHR failed: %d", static_cast<int>(res));
        return false;
    }
    if (res == VK_SUBOPTIMAL_KHR) {
        st.swapchain_out_of_date = true;
    }
    *out_sc_idx = sc_idx;
    *out_wait_sem = acq_sem;
    (void)layer;
    return true;
}

bool mtl_commit_command_buffer_internal(void *cb_obj, bool wait_completed) {
    MtlVkState &st = get_state();
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(cb_obj);
    if (!st.initialized || !cb) return false;
    if (cb->committed) {
        if (wait_completed && cb->status < 4) {
            FrameSyncSlot &slot = st.frames[cb->frame_idx];
            if (slot.submitted) {
                vkWaitForFences(st.vk_ctx.device(), 1, &slot.in_flight_fence, VK_TRUE, UINT64_MAX);
                st.staging.notify_fence_completed(slot.in_flight_fence);
                slot.submitted = false;
                tick_deferred_destroys(false);
            }
            cb->status = 4;
            for (void *blk : cb->completed_blocks) {
                invoke_objc_block_1(blk, cb_obj);
                release_objc_block(blk);
            }
            cb->completed_blocks.clear();
        }
        return true;
    }

    if (!cb->recording || cb->cmd == VK_NULL_HANDLE) {
        /* Empty command buffer without any encoder calls: open/close so fence + present remain valid */
        if (mtl_begin_command_buffer_internal(cb) == VK_NULL_HANDLE) return false;
    }

    if (cb->present_requested && cb->present_tex_obj) {
        auto *tex = unwrap_mtl<MtlTextureImpl>(cb->present_tex_obj);
        if (tex && tex->is_swapchain) {
            VkImageLayout cur_layout = tex->mip_layouts.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : tex->mip_layouts[0];
            if (cur_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                /* Transition swapchain image safely even if nothing drew directly into it */
                transition_image_layout(cb->cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        0, 1, 0, 1, cur_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                cur_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            if (cur_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                transition_image_layout(cb->cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        0, 1, 0, 1, cur_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            }
            if (!tex->mip_layouts.empty()) tex->mip_layouts[0] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            if (cb->present_sc_idx < st.swapchain_layouts.size()) {
                st.swapchain_layouts[cb->present_sc_idx] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            }
        }
    }

    if (vkEndCommandBuffer(cb->cmd) != VK_SUCCESS) {
        MTLVK_LOGE("vkEndCommandBuffer failed");
        return false;
    }
    cb->recording = false;
    cb->committed = true;
    cb->status = 2; /* Committed */

    FrameSyncSlot &slot = st.frames[cb->frame_idx];
    VkSemaphore wait_sems[1] = {cb->acquire_wait_sem};
    VkPipelineStageFlags wait_stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signal_sems[1] = {};
    uint32_t signal_count = 0;
    uint32_t wait_count = (cb->acquire_wait_sem != VK_NULL_HANDLE) ? 1u : 0u;

    if (cb->present_requested && cb->present_sc_idx < st.render_finished_sems.size()) {
        signal_sems[0] = st.render_finished_sems[cb->present_sc_idx];
        signal_count = 1;
    }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = wait_count;
    submit.pWaitSemaphores = wait_count ? wait_sems : nullptr;
    submit.pWaitDstStageMask = wait_count ? wait_stages : nullptr;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb->cmd;
    submit.signalSemaphoreCount = signal_count;
    submit.pSignalSemaphores = signal_count ? signal_sems : nullptr;

    VkResult qres = vkQueueSubmit(st.vk_ctx.graphics_queue(), 1, &submit, slot.in_flight_fence);
    if (qres != VK_SUCCESS) {
        MTLVK_LOGE("vkQueueSubmit failed: %d", static_cast<int>(qres));
        return false;
    }
    slot.submitted = true;
    st.command_buffers_committed.fetch_add(1, std::memory_order_relaxed);

    {
        std::vector<void *> sched;
        sched.swap(cb->scheduled_blocks);
        for (void *blk : sched) {
            invoke_objc_block_1(blk, cb_obj);
            release_objc_block(blk);
        }
    }

    if (cb->present_requested && signal_count == 1) {
        VkSwapchainKHR sc = st.vk_ctx.swapchain();
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &signal_sems[0];
        present.swapchainCount = 1;
        present.pSwapchains = &sc;
        present.pImageIndices = &cb->present_sc_idx;
        VkResult pres = vkQueuePresentKHR(st.vk_ctx.graphics_queue(), &present);
        if (pres == VK_ERROR_OUT_OF_DATE_KHR) {
            st.swapchain_out_of_date = true;
        } else if (pres == VK_SUBOPTIMAL_KHR || pres == VK_SUCCESS) {
            if (pres == VK_SUBOPTIMAL_KHR) {
                st.swapchain_out_of_date = true;
            }
            const uint64_t pcount = st.presents.fetch_add(1, std::memory_order_relaxed) + 1;
            if (pcount == 1 || (pcount % 60) == 0) {
                MTLVK_LOGI("[M5] Present frame #%llu (%ux%u, rc=%d)",
                           static_cast<unsigned long long>(pcount),
                           st.vk_ctx.swapchain_extent().width,
                           st.vk_ctx.swapchain_extent().height,
                           static_cast<int>(pres));
                MTLVK_LOGI("[M5] MemStats: bufs=%lld (%lld MiB) tex=%lld (%lld MiB) cbs=%lld encs=%lld mtlb_hits=%llu misses=%llu blended_psos=%llu/%llu",
                           static_cast<long long>(st.live_buffers.load(std::memory_order_relaxed)),
                           static_cast<long long>(st.live_buffer_bytes.load(std::memory_order_relaxed) / (1024 * 1024)),
                           static_cast<long long>(st.live_textures.load(std::memory_order_relaxed)),
                           static_cast<long long>(st.live_texture_bytes.load(std::memory_order_relaxed) / (1024 * 1024)),
                           static_cast<long long>(st.live_cmdbufs.load(std::memory_order_relaxed)),
                           static_cast<long long>(st.live_encoders.load(std::memory_order_relaxed)),
                           static_cast<unsigned long long>(st.mtlb_cache_hits.load(std::memory_order_relaxed)),
                           static_cast<unsigned long long>(st.mtlb_cache_misses.load(std::memory_order_relaxed)),
                           static_cast<unsigned long long>(st.blended_pipelines_created.load(std::memory_order_relaxed)),
                           static_cast<unsigned long long>(st.graphics_pipelines_created.load(std::memory_order_relaxed)));
            }
        } else {
            MTLVK_LOGE("vkQueuePresentKHR failed: %d", static_cast<int>(pres));
        }
        if (st.shared_layer_obj) {
            auto *layer = unwrap_mtl<MtlMetalLayerImpl>(st.shared_layer_obj);
            if (layer) layer->has_active_drawable = false;
        }
    }

    if (wait_completed || !cb->completed_blocks.empty()) {
        vkWaitForFences(st.vk_ctx.device(), 1, &slot.in_flight_fence, VK_TRUE, UINT64_MAX);
        st.staging.notify_fence_completed(slot.in_flight_fence);
        slot.submitted = false;
        tick_deferred_destroys(false);
        cb->status = 4; /* Completed */
        std::vector<void *> comp;
        comp.swap(cb->completed_blocks);
        for (void *blk : comp) {
            invoke_objc_block_1(blk, cb_obj);
            release_objc_block(blk);
        }
    }
    return true;
}

namespace {

bool create_per_swapchain_sync(MtlVkState &st) {
    VkDevice dev = st.vk_ctx.device();
    for (VkSemaphore s : st.render_finished_sems) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(dev, s, nullptr);
    }
    st.render_finished_sems.clear();
    st.swapchain_layouts.clear();

    const auto &imgs = st.vk_ctx.swapchain_images();
    const auto &views = st.vk_ctx.swapchain_image_views();
    const VkExtent2D ext = st.vk_ctx.swapchain_extent();
    const VkFormat sc_fmt = st.vk_ctx.swapchain_format();
    const uint64_t mtl_fmt = (sc_fmt == VK_FORMAT_B8G8R8A8_UNORM) ? 80 :
                             (sc_fmt == VK_FORMAT_B8G8R8A8_SRGB) ? 81 :
                             (sc_fmt == VK_FORMAT_R8G8B8A8_SRGB) ? 71 : 70;

    const size_t n = imgs.size();
    st.render_finished_sems.resize(n, VK_NULL_HANDLE);
    st.swapchain_layouts.resize(n, VK_IMAGE_LAYOUT_UNDEFINED);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (size_t i = 0; i < n; ++i) {
        if (vkCreateSemaphore(dev, &si, nullptr, &st.render_finished_sems[i]) != VK_SUCCESS) {
            MTLVK_LOGE("Failed creating render_finished_sems[%zu]", i);
            return false;
        }
    }

    if (st.swapchain_tex_objs.size() < n) st.swapchain_tex_objs.resize(n, nullptr);
    if (st.swapchain_drawable_objs.size() < n) st.swapchain_drawable_objs.resize(n, nullptr);
    for (size_t i = 0; i < n; ++i) {
        MtlTextureImpl *tex = nullptr;
        if (!st.swapchain_tex_objs[i]) {
            tex = new MtlTextureImpl();
            st.swapchain_tex_objs[i] = alloc_mtl_wrap("MTLTexture", tex);
            oh2_objc_mark_immortal(st.swapchain_tex_objs[i]);
        } else {
            tex = unwrap_mtl<MtlTextureImpl>(st.swapchain_tex_objs[i]);
        }
        tex->image = imgs[i];
        tex->view = (i < views.size()) ? views[i] : VK_NULL_HANDLE;
        tex->vk_format = sc_fmt;
        tex->mtl_pixel_format = mtl_fmt;
        tex->texture_type = kMTLTextureType2D;
        tex->width = ext.width;
        tex->height = ext.height;
        tex->depth = 1;
        tex->mip_levels = 1;
        tex->array_layers = 1;
        tex->sample_count = 1;
        tex->aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
        tex->owns_image = false;
        tex->owns_view = false;
        tex->is_swapchain = true;
        tex->swapchain_image_idx = static_cast<uint32_t>(i);
        tex->mip_layouts.assign(1, VK_IMAGE_LAYOUT_UNDEFINED);

        MtlDrawableImpl *dr = nullptr;
        if (!st.swapchain_drawable_objs[i]) {
            dr = new MtlDrawableImpl();
            st.swapchain_drawable_objs[i] = alloc_mtl_wrap("CAMetalDrawable", dr);
            oh2_objc_mark_immortal(st.swapchain_drawable_objs[i]);
        } else {
            dr = unwrap_mtl<MtlDrawableImpl>(st.swapchain_drawable_objs[i]);
        }
        dr->texture_obj = st.swapchain_tex_objs[i];
        dr->swapchain_idx = static_cast<uint32_t>(i);
        dr->acquire_sem = VK_NULL_HANDLE;
        dr->acquired = false;
    }
    return true;
}

bool create_dummy_resources(MtlVkState &st) {
    VkDevice dev = st.vk_ctx.device();
    VmaAllocator alloc = st.vk_ctx.allocator();

    /* 1. Dummy 1x1 white RGBA8 image + view */
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {1, 1, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo a_gpu{};
    a_gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(alloc, &ici, &a_gpu, &st.dummy_image, &st.dummy_image_alloc, nullptr) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = st.dummy_image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &ivci, nullptr, &st.dummy_image_view) != VK_SUCCESS) return false;

    /* 2. Dummy 1x1 depth image + view */
    ici.format = st.vk_ctx.caps().d32_sfloat_supported ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vmaCreateImage(alloc, &ici, &a_gpu, &st.dummy_depth_image, &st.dummy_depth_alloc, nullptr) != VK_SUCCESS) {
        return false;
    }
    ivci.image = st.dummy_depth_image;
    ivci.format = ici.format;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &ivci, nullptr, &st.dummy_depth_view) != VK_SUCCESS) return false;

    /* 3. Transition both dummy images */
    execute_immediate_upload([](VkCommandBuffer cmd, void *ud) {
        auto *s = static_cast<MtlVkState *>(ud);
        transition_image_layout(cmd, s->dummy_image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition_image_layout(cmd, s->dummy_depth_image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }, &st);

    /* 4. Dummy sampler */
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 16.0f;
    if (vkCreateSampler(dev, &sci, nullptr, &st.dummy_sampler) != VK_SUCCESS) return false;

    /* 5. Dummy 4 KiB buffer */
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 4096;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo a_cpu{};
    a_cpu.usage = VMA_MEMORY_USAGE_AUTO;
    a_cpu.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (vmaCreateBuffer(alloc, &bci, &a_cpu, &st.dummy_buffer, &st.dummy_buffer_alloc, nullptr) != VK_SUCCESS) {
        return false;
    }
    return true;
}

/* ============================================================================
 * ObjC MTLDevice, MTLLibrary, MTLFunction, MTLCommandQueue, MTLCommandBuffer
 * ============================================================================ */
static void *mtl_create_system_default_device(void) {
    MtlVkState &st = get_state();
    if (!st.device_obj) {
        st.device_obj = alloc_mtl_wrap("MTLDevice", &st);
        oh2_objc_mark_immortal(st.device_obj);
        MTLVK_LOGI("[M5] _MTLCreateSystemDefaultDevice() -> %p (GPU: %s)",
                   st.device_obj, st.vk_ctx.caps().device_name);
    }
    return st.device_obj;
}

static void *mtl_device_name(void *self, const char *sel) {
    (void)self; (void)sel;
    MtlVkState &st = get_state();
    return oh2_objc_create_string(st.vk_ctx.caps().device_name[0] ? st.vk_ctx.caps().device_name : "Samsung Xclipse 540 (Vulkan 1.1)");
}

static uint8_t mtl_device_supportsFeatureSet(void *self, const char *sel, uint64_t featureSet) {
    (void)self; (void)sel;
    /* iOS GPU Family 1..5 / Metal 2.0 feature sets */
    return (featureSet <= 16ULL) ? 1 : 0;
}

static uint8_t mtl_device_supportsFamily(void *self, const char *sel, int64_t gpuFamily) {
    (void)self; (void)sel;
    /* MTLGPUFamilyApple1..5 */
    return (gpuFamily <= 1005) ? 1 : 0;
}

static uint8_t mtl_device_supportsSampleCount(void *self, const char *sel, uint64_t sampleCount) {
    (void)self; (void)sel;
    return (sampleCount == 1) ? 1 : 0;
}

static uint8_t mtl_device_isDepth24Stencil8Supported(void *self, const char *sel) {
    (void)self; (void)sel;
    return 0; /* Xclipse 540 uses Depth32Float / Depth32Float_Stencil8 */
}

static uint8_t mtl_device_hasUnifiedMemory(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1;
}

static MTLSize mtl_device_maxThreadsPerThreadgroup(void *self, const char *sel) {
    (void)self; (void)sel;
    return MTLSize{512, 512, 64};
}

static uint64_t mtl_device_recommendedMaxWorkingSetSize(void *self, const char *sel) {
    (void)self; (void)sel;
    return 2ULL * 1024ULL * 1024ULL * 1024ULL;
}

static uint64_t mtl_device_argumentBuffersSupport(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1; /* MTLArgumentBuffersTier2 = 1 */
}

static uint64_t mtl_device_readWriteTextureSupport(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1;
}

static uint64_t mtl_device_minLinearAlign(void *self, const char *sel, uint64_t pixelFormat) {
    (void)self; (void)sel; (void)pixelFormat;
    return 256;
}

static void mtl_lib_dealloc(void *self) {
    auto *lib = unwrap_mtl<MtlLibraryImpl>(self);
    delete lib;
}

static void mtl_fn_dealloc(void *self) {
    auto *fn = unwrap_mtl<MtlFunctionImpl>(self);
    /* fn->module is cached in MtlVkState::shader_modules; never destroy it here. */
    delete fn;
}

static void mtl_fence_dealloc(void *self) {
    delete unwrap_mtl<MtlFenceImpl>(self);
}

static void mtl_heap_dealloc(void *self) {
    delete unwrap_mtl<MtlHeapImpl>(self);
}

static void mtl_cb_dealloc(void *self) {
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(self);
    if (!cb) return;
    for (void *blk : cb->scheduled_blocks) oh2_objc_release_object(blk);
    for (void *blk : cb->completed_blocks) oh2_objc_release_object(blk);
    get_state().live_cmdbufs.fetch_sub(1, std::memory_order_relaxed);
    delete cb;
}

static void *mtl_device_newCommandQueue(void *self, const char *sel) {
    (void)self; (void)sel;
    MtlVkState &st = get_state();
    if (!st.default_queue_obj) {
        st.default_queue_obj = alloc_mtl_wrap("MTLCommandQueue", &st);
        oh2_objc_mark_immortal(st.default_queue_obj);
    }
    return st.default_queue_obj;
}

static void *mtl_device_newCommandQueueWithMax(void *self, const char *sel, uint64_t maxCount) {
    (void)maxCount;
    return mtl_device_newCommandQueue(self, sel);
}

static void *mtl_device_newDefaultLibrary(void *self, const char *sel) {
    (void)self; (void)sel;
    MtlVkState &st = get_state();
    if (!st.default_library_obj) {
        auto *impl = new MtlLibraryImpl();
        impl->is_default_library = true;
        impl->label = "DefaultLibrary";
        st.default_library_obj = alloc_mtl_wrap("MTLLibrary", impl);
        oh2_objc_mark_immortal(st.default_library_obj);
    }
    return st.default_library_obj;
}

static void *mtl_device_newDefaultLibraryWithBundle(void *self, const char *sel, void *bundle, void **err) {
    (void)bundle;
    if (err) *err = nullptr;
    return mtl_device_newDefaultLibrary(self, sel);
}

static void *mtl_device_newLibraryWithData(void *self, const char *sel, void *data, void **err) {
    (void)self; (void)sel;
    if (err) *err = nullptr;
    if (!data) {
        if (err) *err = create_ns_error("nil shader library data");
        return nullptr;
    }
    const void *bytes = oh2_objc_data_bytes(data);
    size_t len = oh2_objc_data_length(data);
    const SpirvShaderEntry *entry = (bytes && len >= 4) ? spirv_cache_find_by_mtlb(bytes, len) : nullptr;
    if (!entry && bytes && len >= 88) {
        const uint8_t *p = static_cast<const uint8_t *>(bytes);
        for (size_t off = 0; off + 88 <= len; off += 4) {
            if (p[off] == 'M' && p[off + 1] == 'T' && p[off + 2] == 'L' && p[off + 3] == 'B') {
                const uint8_t *mtlb = p + off;
                size_t rem = len - off;
                uint64_t total_sz = 0;
                std::memcpy(&total_sz, mtlb + 16, sizeof(total_sz));
                if (total_sz >= 88 && total_sz <= rem) {
                    entry = spirv_cache_find_by_mtlb(mtlb, static_cast<size_t>(total_sz));
                }
                if (!entry) {
                    entry = spirv_cache_find_by_mtlb(mtlb, rem);
                }
                if (!entry) {
                    uint64_t fn_off = 0, fn_sz = 0;
                    std::memcpy(&fn_off, mtlb + 24, sizeof(fn_off));
                    std::memcpy(&fn_sz, mtlb + 32, sizeof(fn_sz));
                    if (fn_off + 8 <= rem && fn_sz > 0) {
                        size_t fn_end = std::min(rem, static_cast<size_t>(fn_off + fn_sz));
                        if (fn_off + 8 <= fn_end) {
                            /* Skip 8-byte MTLB function-list header (count + group_size) before reading tags */
                            size_t q = static_cast<size_t>(fn_off) + 8;
                            while (q + 6 <= fn_end) {
                                if (mtlb[q] == 'E' && mtlb[q + 1] == 'N' && mtlb[q + 2] == 'D' && mtlb[q + 3] == 'T') break;
                                bool is_name = (mtlb[q] == 'N' && mtlb[q + 1] == 'A' && mtlb[q + 2] == 'M' && mtlb[q + 3] == 'E');
                                uint16_t tlen = 0;
                                std::memcpy(&tlen, mtlb + q + 4, sizeof(tlen));
                                q += 6;
                                if (q + tlen > fn_end) break;
                                if (is_name && tlen > 1) {
                                    std::string fn_name(reinterpret_cast<const char *>(mtlb + q), tlen);
                                    size_t nul = fn_name.find('\0');
                                    if (nul != std::string::npos) fn_name.resize(nul);
                                    if (!fn_name.empty()) entry = spirv_cache_find_by_entry_name(fn_name.c_str());
                                    if (entry) break;
                                }
                                q += tlen;
                            }
                        }
                    }
                }
                if (entry) break;
            }
        }
    }
    auto *impl = new MtlLibraryImpl();
    impl->single_entry = entry;
    impl->is_default_library = false;
    MtlVkState &st = get_state();
    if (entry) {
        st.mtlb_cache_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
        uint64_t misses = st.mtlb_cache_misses.fetch_add(1, std::memory_order_relaxed) + 1;
        MTLVK_LOGW("newLibraryWithData: MTLB not matched in spirv_cache (len=%zu, misses=%llu)",
                   len, static_cast<unsigned long long>(misses));
    }
    return alloc_mtl_wrap_managed("MTLLibrary", impl, &mtl_lib_dealloc, false);
}

static void *mtl_device_newLibraryWithFile(void *self, const char *sel, void *path_cf, void **err) {
    (void)path_cf;
    if (err) *err = nullptr;
    return mtl_device_newDefaultLibrary(self, sel);
}

static void *mtl_device_newFence(void *self, const char *sel) {
    (void)self; (void)sel;
    return alloc_mtl_wrap_managed("MTLFence", new MtlFenceImpl(), &mtl_fence_dealloc, false);
}

static void *mtl_device_newHeap(void *self, const char *sel, void *desc_obj) {
    (void)self; (void)sel;
    auto *h = new MtlHeapImpl();
    if (desc_obj) {
        auto *hd = unwrap_mtl<MtlHeapImpl>(desc_obj);
        if (hd) {
            h->size = hd->size;
            h->storage_mode = hd->storage_mode;
        }
    }
    return alloc_mtl_wrap_managed("MTLHeap", h, &mtl_heap_dealloc, false);
}

static MTLSizeAndAlign mtl_device_heapBufferSizeAndAlign(void *self, const char *sel, uint64_t len, uint64_t opts) {
    (void)self; (void)sel; (void)opts;
    uint64_t aligned = (len + 255ULL) & ~255ULL;
    return MTLSizeAndAlign{aligned, 256ULL};
}

static MTLSizeAndAlign mtl_device_heapTextureSizeAndAlign(void *self, const char *sel, void *desc_obj) {
    (void)self; (void)sel;
    auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(desc_obj);
    uint64_t w = d ? d->width : 64;
    uint64_t h = d ? d->height : 64;
    return MTLSizeAndAlign{(w * h * 4ULL + 4095ULL) & ~4095ULL, 4096ULL};
}

static void *mtl_lib_newFunctionWithName(void *self, const char *sel, void *name_ns) {
    (void)sel;
    auto *lib = unwrap_mtl<MtlLibraryImpl>(self);
    std::string fname = nsstr_to_std(name_ns);
    const SpirvShaderEntry *entry = nullptr;
    if (lib && lib->single_entry) {
        entry = lib->single_entry;
    } else if (!fname.empty()) {
        entry = spirv_cache_find_by_entry_name(fname.c_str());
    }

    auto *fn = new MtlFunctionImpl();
    fn->entry = entry;
    fn->name = fname.empty() && entry && entry->entry_name ? entry->entry_name : fname;
    if (entry) {
        fn->function_type = (entry->stage == SPIRV_STAGE_FRAGMENT) ? 2 :
                            (entry->stage == SPIRV_STAGE_COMPUTE)  ? 3 : 1;
        fn->module = get_or_create_shader_module(entry);
    }
    return alloc_mtl_wrap_managed("MTLFunction", fn, &mtl_fn_dealloc, false);
}

static void *mtl_lib_newFunctionWithConst(void *self, const char *sel, void *name_ns, void *constVals, void **err) {
    (void)constVals;
    if (err) *err = nullptr;
    return mtl_lib_newFunctionWithName(self, sel, name_ns);
}

static void *mtl_lib_functionNames(void *self, const char *sel) {
    (void)sel;
    auto *lib = unwrap_mtl<MtlLibraryImpl>(self);
    if (lib && lib->single_entry && lib->single_entry->entry_name) {
        const void *s = oh2_objc_create_string(lib->single_entry->entry_name);
        return oh2_objc_array_create(&s, 1);
    }
    return oh2_objc_array_create(nullptr, 0);
}

static void *mtl_fn_name(void *self, const char *sel) {
    (void)sel;
    auto *fn = unwrap_mtl<MtlFunctionImpl>(self);
    return oh2_objc_create_string((fn && !fn->name.empty()) ? fn->name.c_str() : "main");
}

static uint64_t mtl_fn_type(void *self, const char *sel) {
    (void)sel;
    auto *fn = unwrap_mtl<MtlFunctionImpl>(self);
    return fn ? fn->function_type : 1ULL;
}

/* MTLCommandQueue & MTLCommandBuffer */
static void *mtl_queue_commandBuffer(void *self, const char *sel) {
    (void)self; (void)sel;
    auto *cb = new MtlCommandBufferImpl();
    get_state().live_cmdbufs.fetch_add(1, std::memory_order_relaxed);
    return alloc_mtl_wrap_managed("MTLCommandBuffer", cb, &mtl_cb_dealloc, true);
}

static void mtl_cb_addScheduled(void *self, const char *sel, void *blk) {
    (void)sel;
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(self);
    if (cb && blk) cb->scheduled_blocks.push_back(copy_objc_block(blk));
}

static void mtl_cb_addCompleted(void *self, const char *sel, void *blk) {
    (void)sel;
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(self);
    if (cb && blk) cb->completed_blocks.push_back(copy_objc_block(blk));
}

static void mtl_cb_presentDrawable(void *self, const char *sel, void *drawable_obj) {
    (void)sel;
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(self);
    auto *dr = unwrap_mtl<MtlDrawableImpl>(drawable_obj);
    if (!cb || !dr) return;
    cb->acquire_wait_sem = dr->acquire_sem;
    dr->acquire_sem = VK_NULL_HANDLE;
    cb->present_requested = true;
    cb->present_sc_idx = dr->swapchain_idx;
    cb->present_tex_obj = dr->texture_obj;
}

static void mtl_cb_presentDrawableAtTime(void *self, const char *sel, void *drawable_obj, double time) {
    (void)time;
    mtl_cb_presentDrawable(self, sel, drawable_obj);
}

static void mtl_cb_commit(void *self, const char *sel) {
    (void)sel;
    mtl_commit_command_buffer_internal(self, false);
}

static void mtl_cb_waitUntilCompleted(void *self, const char *sel) {
    (void)sel;
    mtl_commit_command_buffer_internal(self, true);
}

static uint64_t mtl_cb_status(void *self, const char *sel) {
    (void)sel;
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(self);
    return cb ? cb->status : 4ULL;
}

static void *mtl_cb_error(void *self, const char *sel) {
    (void)self; (void)sel;
    return nullptr;
}

static void mtl_cb_noop(void *self, const char *sel) { (void)self; (void)sel; }
static void mtl_cb_setLabel(void *self, const char *sel, void *lbl) { (void)self; (void)sel; (void)lbl; }
static void *mtl_cb_label(void *self, const char *sel) { (void)self; (void)sel; return oh2_objc_create_string("MTL"); }
static uint8_t mtl_cb_retained(void *self, const char *sel) { (void)self; (void)sel; return 1; }
static double mtl_cb_zeroTime(void *self, const char *sel) { (void)self; (void)sel; return 0.0; }
static void *mtl_cb_parallelEncoder(void *self, const char *sel, void *desc) {
    (void)sel;
    typedef void *(*fn_t)(void *, const char *, void *);
    return ((fn_t)oh2_objc_msgSend)(self, "renderCommandEncoderWithDescriptor:", desc);
}

static void *mtl_heap_size(void *self, const char *sel) { (void)sel; auto *h = unwrap_mtl<MtlHeapImpl>(self); return (void *)(uintptr_t)(h ? h->size : 0); }
static void mtl_heap_setSize(void *self, const char *sel, uint64_t sz) { (void)sel; auto *h = unwrap_mtl<MtlHeapImpl>(self); if (h) h->size = sz; }
static uint64_t mtl_heap_storageMode(void *self, const char *sel) { (void)sel; auto *h = unwrap_mtl<MtlHeapImpl>(self); return h ? h->storage_mode : 2ULL; }
static uint64_t mtl_heap_usedSize(void *self, const char *sel) { (void)sel; auto *h = unwrap_mtl<MtlHeapImpl>(self); return h ? h->used : 0ULL; }
static uint64_t mtl_heap_maxAvail(void *self, const char *sel, uint64_t align) {
    (void)sel; (void)align;
    auto *h = unwrap_mtl<MtlHeapImpl>(self);
    return (h && h->size > h->used) ? (h->size - h->used) : 0ULL;
}
static void *mtl_heap_newBuf(void *self, const char *sel, uint64_t len, uint64_t opts) {
    (void)self; (void)sel;
    typedef void *(*fn_t)(void *, const char *, uint64_t, uint64_t);
    return ((fn_t)oh2_objc_msgSend)(get_state().device_obj, "newBufferWithLength:options:", len, opts);
}
static void *mtl_heap_newTex(void *self, const char *sel, void *desc) {
    (void)self; (void)sel;
    typedef void *(*fn_t)(void *, const char *, void *);
    return ((fn_t)oh2_objc_msgSend)(get_state().device_obj, "newTextureWithDescriptor:", desc);
}

static uint64_t mtl_device_registryID(void *self, const char *sel) { (void)self; (void)sel; return 0x1001ULL; }
static uint8_t  mtl_device_false(void *self, const char *sel) { (void)self; (void)sel; return 0; }
static uint64_t mtl_device_currentAllocatedSize(void *self, const char *sel) { (void)self; (void)sel; return 0; }
static uint64_t mtl_device_maxThreadgroupMemory(void *self, const char *sel) { (void)self; (void)sel; return 32768ULL; }
static void *mtl_obj_device(void *self, const char *sel) { (void)self; (void)sel; return get_state().device_obj; }

static void *g_capture_manager = nullptr;
static void *g_default_capture_scope = nullptr;

static void *mtl_cap_sharedManager(void *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_capture_manager) {
        g_capture_manager = oh2_objc_alloc_instance(oh2_objc_get_class("MTLCaptureManager"), 64);
        oh2_objc_mark_immortal(g_capture_manager);
    }
    return g_capture_manager;
}
static void *mtl_cap_newScopeWithDev(void *self, const char *sel, void *dev) {
    (void)self; (void)sel; (void)dev;
    return oh2_objc_alloc_instance(oh2_objc_get_class("MTLCaptureScope"), 64);
}
static void *mtl_cap_defaultScope(void *self, const char *sel) {
    (void)self; (void)sel;
    if (!g_default_capture_scope) {
        g_default_capture_scope = oh2_objc_alloc_instance(oh2_objc_get_class("MTLCaptureScope"), 64);
        oh2_objc_mark_immortal(g_default_capture_scope);
    }
    return g_default_capture_scope;
}
static void mtl_cap_setDefaultScope(void *self, const char *sel, void *scope) {
    (void)self; (void)sel;
    g_default_capture_scope = scope;
}
static uint8_t mtl_cap_false1(void *self, const char *sel, uint64_t a) { (void)self; (void)sel; (void)a; return 0; }
static void mtl_scope_noop(void *self, const char *sel) { (void)self; (void)sel; }
static void *mtl_scope_queue(void *self, const char *sel) { (void)self; (void)sel; return get_state().default_queue_obj; }

/* Async completion & auxiliary Metal objects (M5 batch) */
struct BlockLayout2 {
    void *isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke)(void *, void *, void *);
};
static void invoke_objc_block_2(void *block, void *arg1, void *arg2) {
    if (!block) return;
    auto *b = static_cast<BlockLayout2 *>(block);
    if (b->invoke) b->invoke(block, arg1, arg2);
}

static void mtl_device_newRenderPSOAsync(void *self, const char *sel, void *desc, void *handler) {
    (void)sel;
    typedef void *(*fn_t)(void *, const char *, void *, void **);
    void *err = nullptr;
    void *pso = ((fn_t)oh2_objc_msgSend)(self, "newRenderPipelineStateWithDescriptor:error:", desc, &err);
    invoke_objc_block_2(handler, pso, err);
}
static void mtl_device_newRenderPSOOptsAsync(void *self, const char *sel, void *desc, uint64_t opts, void *handler) {
    (void)opts;
    mtl_device_newRenderPSOAsync(self, sel, desc, handler);
}
static void mtl_device_newComputeFnAsync(void *self, const char *sel, void *fn, void *handler) {
    (void)sel;
    typedef void *(*fn_t)(void *, const char *, void *, void **);
    void *err = nullptr;
    void *pso = ((fn_t)oh2_objc_msgSend)(self, "newComputePipelineStateWithFunction:error:", fn, &err);
    invoke_objc_block_2(handler, pso, err);
}
static void mtl_device_newComputeFnOptsAsync(void *self, const char *sel, void *fn, uint64_t opts, void *handler) {
    (void)opts;
    mtl_device_newComputeFnAsync(self, sel, fn, handler);
}
static void mtl_device_newComputeDescAsync(void *self, const char *sel, void *desc, uint64_t opts, void *handler) {
    (void)sel;
    typedef void *(*fn_t)(void *, const char *, void *, uint64_t, void **, void **);
    void *err = nullptr;
    void *pso = ((fn_t)oh2_objc_msgSend)(self, "newComputePipelineStateWithDescriptor:options:reflection:error:", desc, opts, nullptr, &err);
    invoke_objc_block_2(handler, pso, err);
}
static void *mtl_device_newLibraryWithSource(void *self, const char *sel, void *src, void *opts, void **err) {
    (void)src; (void)opts;
    if (err) *err = nullptr;
    return mtl_device_newDefaultLibrary(self, sel);
}
static void mtl_device_newLibraryWithSourceAsync(void *self, const char *sel, void *src, void *opts, void *handler) {
    void *err = nullptr;
    void *lib = mtl_device_newLibraryWithSource(self, sel, src, opts, &err);
    invoke_objc_block_2(handler, lib, err);
}
static void *mtl_device_newTextureIOSurface(void *self, const char *sel, void *desc, void *iosurf, uint64_t plane) {
    (void)sel; (void)iosurf; (void)plane;
    typedef void *(*fn_t)(void *, const char *, void *);
    return ((fn_t)oh2_objc_msgSend)(self, "newTextureWithDescriptor:", desc);
}
static void *mtl_device_newArgEncoder(void *self, const char *sel, void *args) { (void)self; (void)sel; (void)args; return nullptr; }
static void *mtl_device_newTilePSOSync(void *self, const char *sel, void *d, uint64_t o, void **r, void **e) {
    (void)self; (void)sel; (void)d; (void)o;
    if (r) *r = nullptr; if (e) *e = nullptr;
    return nullptr;
}
static void mtl_device_newTilePSOAsync(void *self, const char *sel, void *d, uint64_t o, void *handler) {
    (void)self; (void)sel; (void)d; (void)o;
    invoke_objc_block_2(handler, nullptr, nullptr);
}
static void mtl_device_defaultSamplePos(void *self, const char *sel, float *pos, uint64_t count) {
    (void)self; (void)sel;
    if (pos && count > 0) {
        for (uint64_t i = 0; i < count; ++i) { pos[i * 2] = 0.5f; pos[i * 2 + 1] = 0.5f; }
    }
}
static void mtl_lib_newFunctionConstAsync(void *self, const char *sel, void *name_ns, void *constVals, void *handler) {
    void *err = nullptr;
    void *fn = mtl_lib_newFunctionWithConst(self, sel, name_ns, constVals, &err);
    invoke_objc_block_2(handler, fn, err);
}
static void *mtl_fn_emptyArray(void *s, const char *sel) { (void)s; (void)sel; return oh2_objc_array_create(nullptr, 0); }
static void *mtl_fn_emptyDict(void *s, const char *sel) { (void)s; (void)sel; return oh2_objc_dict_create(); }
static void *mtl_fn_newArgEnc1(void *s, const char *sel, uint64_t idx) { (void)s; (void)sel; (void)idx; return nullptr; }
static void *mtl_fn_newArgEnc2(void *s, const char *sel, uint64_t idx, void **refl) { (void)s; (void)sel; (void)idx; if (refl) *refl = nullptr; return nullptr; }
static void *mtl_obj_ret_self(void *s, const char *sel) { (void)sel; return s; }
static uint64_t mtl_obj_zero_u64(void *s, const char *sel) { (void)s; (void)sel; return 0ULL; }
static uint8_t  mtl_obj_one_u8(void *s, const char *sel) { (void)s; (void)sel; return 1; }
static void *mtl_obj_zero_ptr(void *s, const char *sel) { (void)s; (void)sel; return nullptr; }
static void mtl_obj_noop1_u8(void *s, const char *sel, uint8_t v) { (void)s; (void)sel; (void)v; }
static void mtl_obj_noop1_u64(void *s, const char *sel, uint64_t v) { (void)s; (void)sel; (void)v; }
static void mtl_obj_noop3_const(void *s, const char *sel, const void *v, uint64_t t, uint64_t i) { (void)s; (void)sel; (void)v; (void)t; (void)i; }
static void mtl_obj_noop3_constName(void *s, const char *sel, const void *v, uint64_t t, void *n) { (void)s; (void)sel; (void)v; (void)t; (void)n; }
static void mtl_obj_noop3_constRange(void *s, const char *sel, const void *v, uint64_t t, NSRange64 r) { (void)s; (void)sel; (void)v; (void)t; (void)r; }
static void *mtl_pb_arr_subscript(void *s, const char *sel, uint64_t idx) {
    (void)s; (void)sel; (void)idx;
    return oh2_objc_alloc_instance(oh2_objc_ensure_class("MTLPipelineBufferDescriptor", "NSObject"), 64);
}

} // namespace

void register_device_objc_methods() {
    oh2_objc_ensure_class("MTLDevice", "NSObject");
    oh2_objc_ensure_class("MTLCommandQueue", "NSObject");
    oh2_objc_ensure_class("MTLCommandBuffer", "NSObject");
    oh2_objc_ensure_class("MTLLibrary", "NSObject");
    oh2_objc_ensure_class("MTLFunction", "NSObject");
    oh2_objc_ensure_class("MTLFence", "NSObject");
    oh2_objc_ensure_class("MTLHeap", "NSObject");
    oh2_objc_ensure_class("MTLCaptureManager", "NSObject");
    oh2_objc_ensure_class("MTLCaptureScope", "NSObject");

    oh2_objc_register_instance_method("MTLDevice", "name",                                     (void *)&mtl_device_name);
    oh2_objc_register_instance_method("MTLDevice", "registryID",                               (void *)&mtl_device_registryID);
    oh2_objc_register_instance_method("MTLDevice", "isLowPower",                               (void *)&mtl_device_false);
    oh2_objc_register_instance_method("MTLDevice", "isHeadless",                               (void *)&mtl_device_false);
    oh2_objc_register_instance_method("MTLDevice", "isRemovable",                              (void *)&mtl_device_false);
    oh2_objc_register_instance_method("MTLDevice", "areRasterOrderGroupsSupported",            (void *)&mtl_device_false);
    oh2_objc_register_instance_method("MTLDevice", "areProgrammableSamplePositionsSupported",  (void *)&mtl_device_false);
    oh2_objc_register_instance_method("MTLDevice", "currentAllocatedSize",                     (void *)&mtl_device_currentAllocatedSize);
    oh2_objc_register_instance_method("MTLDevice", "maxThreadgroupMemoryLength",               (void *)&mtl_device_maxThreadgroupMemory);
    oh2_objc_register_instance_method("MTLDevice", "supportsFeatureSet:",                      (void *)&mtl_device_supportsFeatureSet);
    oh2_objc_register_instance_method("MTLDevice", "supportsFamily:",                          (void *)&mtl_device_supportsFamily);
    oh2_objc_register_instance_method("MTLDevice", "supportsTextureSampleCount:",              (void *)&mtl_device_supportsSampleCount);
    oh2_objc_register_instance_method("MTLDevice", "isDepth24Stencil8PixelFormatSupported",    (void *)&mtl_device_isDepth24Stencil8Supported);
    oh2_objc_register_instance_method("MTLDevice", "hasUnifiedMemory",                         (void *)&mtl_device_hasUnifiedMemory);
    oh2_objc_register_instance_method("MTLDevice", "maxThreadsPerThreadgroup",                 (void *)&mtl_device_maxThreadsPerThreadgroup);
    oh2_objc_register_instance_method("MTLDevice", "recommendedMaxWorkingSetSize",             (void *)&mtl_device_recommendedMaxWorkingSetSize);
    oh2_objc_register_instance_method("MTLDevice", "argumentBuffersSupport",                   (void *)&mtl_device_argumentBuffersSupport);
    oh2_objc_register_instance_method("MTLDevice", "readWriteTextureSupport",                  (void *)&mtl_device_readWriteTextureSupport);
    oh2_objc_register_instance_method("MTLDevice", "minimumLinearTextureAlignmentForPixelFormat:", (void *)&mtl_device_minLinearAlign);
    oh2_objc_register_instance_method("MTLDevice", "newCommandQueue",                          (void *)&mtl_device_newCommandQueue);
    oh2_objc_register_instance_method("MTLDevice", "newCommandQueueWithMaxCommandBufferCount:", (void *)&mtl_device_newCommandQueueWithMax);
    oh2_objc_register_instance_method("MTLDevice", "newDefaultLibrary",                        (void *)&mtl_device_newDefaultLibrary);
    oh2_objc_register_instance_method("MTLDevice", "newDefaultLibraryWithBundle:error:",       (void *)&mtl_device_newDefaultLibraryWithBundle);
    oh2_objc_register_instance_method("MTLDevice", "newLibraryWithData:error:",                (void *)&mtl_device_newLibraryWithData);
    oh2_objc_register_instance_method("MTLDevice", "newLibraryWithFile:error:",                (void *)&mtl_device_newLibraryWithFile);
    oh2_objc_register_instance_method("MTLDevice", "newLibraryWithURL:error:",                 (void *)&mtl_device_newLibraryWithFile);
    oh2_objc_register_instance_method("MTLDevice", "newFence",                                 (void *)&mtl_device_newFence);
    oh2_objc_register_instance_method("MTLDevice", "newHeapWithDescriptor:",                   (void *)&mtl_device_newHeap);
    oh2_objc_register_instance_method("MTLDevice", "heapBufferSizeAndAlignWithLength:options:",(void *)&mtl_device_heapBufferSizeAndAlign);
    oh2_objc_register_instance_method("MTLDevice", "heapTextureSizeAndAlignWithDescriptor:",   (void *)&mtl_device_heapTextureSizeAndAlign);
    oh2_objc_register_instance_method("MTLDevice", "newRenderPipelineStateWithDescriptor:completionHandler:", (void *)&mtl_device_newRenderPSOAsync);
    oh2_objc_register_instance_method("MTLDevice", "newRenderPipelineStateWithDescriptor:options:completionHandler:", (void *)&mtl_device_newRenderPSOOptsAsync);
    oh2_objc_register_instance_method("MTLDevice", "newComputePipelineStateWithFunction:completionHandler:", (void *)&mtl_device_newComputeFnAsync);
    oh2_objc_register_instance_method("MTLDevice", "newComputePipelineStateWithFunction:options:completionHandler:", (void *)&mtl_device_newComputeFnOptsAsync);
    oh2_objc_register_instance_method("MTLDevice", "newComputePipelineStateWithDescriptor:options:completionHandler:", (void *)&mtl_device_newComputeDescAsync);
    oh2_objc_register_instance_method("MTLDevice", "newLibraryWithSource:options:error:",      (void *)&mtl_device_newLibraryWithSource);
    oh2_objc_register_instance_method("MTLDevice", "newLibraryWithSource:options:completionHandler:", (void *)&mtl_device_newLibraryWithSourceAsync);
    oh2_objc_register_instance_method("MTLDevice", "newTextureWithDescriptor:iosurface:plane:",(void *)&mtl_device_newTextureIOSurface);
    oh2_objc_register_instance_method("MTLDevice", "newArgumentEncoderWithArguments:",         (void *)&mtl_device_newArgEncoder);
    oh2_objc_register_instance_method("MTLDevice", "getDefaultSamplePositions:count:",         (void *)&mtl_device_defaultSamplePos);
    oh2_objc_register_instance_method("MTLDevice", "newRenderPipelineStateWithTileDescriptor:options:reflection:error:", (void *)&mtl_device_newTilePSOSync);
    oh2_objc_register_instance_method("MTLDevice", "newRenderPipelineStateWithTileDescriptor:options:completionHandler:", (void *)&mtl_device_newTilePSOAsync);

    oh2_objc_register_instance_method("MTLLibrary", "device",                                  (void *)&mtl_obj_device);
    oh2_objc_register_instance_method("MTLLibrary", "label",                                   (void *)&mtl_cb_label);
    oh2_objc_register_instance_method("MTLLibrary", "newFunctionWithName:",                    (void *)&mtl_lib_newFunctionWithName);
    oh2_objc_register_instance_method("MTLLibrary", "newFunctionWithName:constantValues:error:",(void *)&mtl_lib_newFunctionWithConst);
    oh2_objc_register_instance_method("MTLLibrary", "newFunctionWithName:constantValues:completionHandler:", (void *)&mtl_lib_newFunctionConstAsync);
    oh2_objc_register_instance_method("MTLLibrary", "functionNames",                           (void *)&mtl_lib_functionNames);
    oh2_objc_register_instance_method("MTLLibrary", "setLabel:",                               (void *)&mtl_cb_setLabel);

    oh2_objc_register_instance_method("MTLFunction", "device",                                 (void *)&mtl_obj_device);
    oh2_objc_register_instance_method("MTLFunction", "label",                                  (void *)&mtl_cb_label);
    oh2_objc_register_instance_method("MTLFunction", "name",                                   (void *)&mtl_fn_name);
    oh2_objc_register_instance_method("MTLFunction", "functionType",                           (void *)&mtl_fn_type);
    oh2_objc_register_instance_method("MTLFunction", "patchType",                              (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLFunction", "patchControlPointCount",                 (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLFunction", "vertexAttributes",                       (void *)&mtl_fn_emptyArray);
    oh2_objc_register_instance_method("MTLFunction", "stageInputAttributes",                   (void *)&mtl_fn_emptyArray);
    oh2_objc_register_instance_method("MTLFunction", "functionConstantsDictionary",            (void *)&mtl_fn_emptyDict);
    oh2_objc_register_instance_method("MTLFunction", "newArgumentEncoderWithBufferIndex:",     (void *)&mtl_fn_newArgEnc1);
    oh2_objc_register_instance_method("MTLFunction", "newArgumentEncoderWithBufferIndex:reflection:", (void *)&mtl_fn_newArgEnc2);
    oh2_objc_register_instance_method("MTLFunction", "setLabel:",                              (void *)&mtl_cb_setLabel);

    oh2_objc_register_instance_method("MTLCommandQueue", "commandBuffer",                      (void *)&mtl_queue_commandBuffer);
    oh2_objc_register_instance_method("MTLCommandQueue", "commandBufferWithUnretainedReferences", (void *)&mtl_queue_commandBuffer);
    oh2_objc_register_instance_method("MTLCommandQueue", "device",                             (void *)&mtl_obj_device);
    oh2_objc_register_instance_method("MTLCommandQueue", "label",                              (void *)&mtl_cb_label);
    oh2_objc_register_instance_method("MTLCommandQueue", "setLabel:",                          (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLCommandQueue", "insertDebugCaptureBoundary",         (void *)&mtl_cb_noop);

    oh2_objc_register_instance_method("MTLFence",        "device",                             (void *)&mtl_obj_device);
    oh2_objc_register_instance_method("MTLFence",        "label",                              (void *)&mtl_cb_label);
    oh2_objc_register_instance_method("MTLFence",        "setLabel:",                          (void *)&mtl_cb_setLabel);

    oh2_objc_register_class_method("MTLCaptureManager",    "sharedCaptureManager",             (void *)&mtl_cap_sharedManager);
    oh2_objc_register_instance_method("MTLCaptureManager", "newCaptureScopeWithDevice:",       (void *)&mtl_cap_newScopeWithDev);
    oh2_objc_register_instance_method("MTLCaptureManager", "newCaptureScopeWithCommandQueue:", (void *)&mtl_cap_newScopeWithDev);
    oh2_objc_register_instance_method("MTLCaptureManager", "defaultCaptureScope",              (void *)&mtl_cap_defaultScope);
    oh2_objc_register_instance_method("MTLCaptureManager", "setDefaultCaptureScope:",          (void *)&mtl_cap_setDefaultScope);
    oh2_objc_register_instance_method("MTLCaptureManager", "isCapturing",                      (void *)&mtl_device_false);
    oh2_objc_register_instance_method("MTLCaptureManager", "supportsDestination:",             (void *)&mtl_cap_false1);
    oh2_objc_register_instance_method("MTLCaptureManager", "startCaptureWithDevice:",          (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLCaptureManager", "startCaptureWithCommandQueue:",    (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLCaptureManager", "startCaptureWithScope:",           (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLCaptureManager", "stopCapture",                      (void *)&mtl_cb_noop);

    oh2_objc_register_instance_method("MTLCaptureScope",   "beginScope",                       (void *)&mtl_scope_noop);
    oh2_objc_register_instance_method("MTLCaptureScope",   "endScope",                         (void *)&mtl_scope_noop);
    oh2_objc_register_instance_method("MTLCaptureScope",   "label",                            (void *)&mtl_cb_label);
    oh2_objc_register_instance_method("MTLCaptureScope",   "setLabel:",                        (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLCaptureScope",   "device",                           (void *)&mtl_obj_device);
    oh2_objc_register_instance_method("MTLCaptureScope",   "commandQueue",                     (void *)&mtl_scope_queue);

    oh2_objc_register_instance_method("MTLCommandBuffer", "addScheduledHandler:",              (void *)&mtl_cb_addScheduled);
    oh2_objc_register_instance_method("MTLCommandBuffer", "addCompletedHandler:",              (void *)&mtl_cb_addCompleted);
    oh2_objc_register_instance_method("MTLCommandBuffer", "presentDrawable:",                  (void *)&mtl_cb_presentDrawable);
    oh2_objc_register_instance_method("MTLCommandBuffer", "presentDrawable:atTime:",           (void *)&mtl_cb_presentDrawableAtTime);
    oh2_objc_register_instance_method("MTLCommandBuffer", "presentDrawable:afterMinimumDuration:", (void *)&mtl_cb_presentDrawableAtTime);
    oh2_objc_register_instance_method("MTLCommandBuffer", "commit",                            (void *)&mtl_cb_commit);
    oh2_objc_register_instance_method("MTLCommandBuffer", "waitUntilScheduled",                (void *)&mtl_cb_noop);
    oh2_objc_register_instance_method("MTLCommandBuffer", "waitUntilCompleted",                (void *)&mtl_cb_waitUntilCompleted);
    oh2_objc_register_instance_method("MTLCommandBuffer", "status",                            (void *)&mtl_cb_status);
    oh2_objc_register_instance_method("MTLCommandBuffer", "error",                             (void *)&mtl_cb_error);
    oh2_objc_register_instance_method("MTLCommandBuffer", "label",                             (void *)&mtl_cb_label);
    oh2_objc_register_instance_method("MTLCommandBuffer", "setLabel:",                         (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLCommandBuffer", "device",                            (void *)&mtl_obj_device);
    oh2_objc_register_instance_method("MTLCommandBuffer", "commandQueue",                      (void *)&mtl_scope_queue);
    oh2_objc_register_instance_method("MTLCommandBuffer", "retainedReferences",                (void *)&mtl_cb_retained);
    oh2_objc_register_instance_method("MTLCommandBuffer", "kernelStartTime",                   (void *)&mtl_cb_zeroTime);
    oh2_objc_register_instance_method("MTLCommandBuffer", "kernelEndTime",                     (void *)&mtl_cb_zeroTime);
    oh2_objc_register_instance_method("MTLCommandBuffer", "GPUStartTime",                      (void *)&mtl_cb_zeroTime);
    oh2_objc_register_instance_method("MTLCommandBuffer", "GPUEndTime",                        (void *)&mtl_cb_zeroTime);
    oh2_objc_register_instance_method("MTLCommandBuffer", "enqueue",                           (void *)&mtl_cb_noop);
    oh2_objc_register_instance_method("MTLCommandBuffer", "parallelRenderCommandEncoderWithDescriptor:", (void *)&mtl_cb_parallelEncoder);
    oh2_objc_register_instance_method("MTLCommandBuffer", "pushDebugGroup:",                   (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLCommandBuffer", "popDebugGroup",                     (void *)&mtl_cb_noop);

    oh2_objc_register_instance_method("MTLHeapDescriptor", "size",                             (void *)&mtl_heap_size);
    oh2_objc_register_instance_method("MTLHeapDescriptor", "setSize:",                         (void *)&mtl_heap_setSize);
    oh2_objc_register_instance_method("MTLHeapDescriptor", "storageMode",                      (void *)&mtl_heap_storageMode);
    oh2_objc_register_instance_method("MTLHeap",           "size",                             (void *)&mtl_heap_size);
    oh2_objc_register_instance_method("MTLHeap",           "device",                           (void *)&mtl_obj_device);
    oh2_objc_register_instance_method("MTLHeap",           "storageMode",                      (void *)&mtl_heap_storageMode);
    oh2_objc_register_instance_method("MTLHeap",           "cpuCacheMode",                     (void *)&mtl_device_currentAllocatedSize);
    oh2_objc_register_instance_method("MTLHeap",           "usedSize",                         (void *)&mtl_heap_usedSize);
    oh2_objc_register_instance_method("MTLHeap",           "currentAllocatedSize",             (void *)&mtl_heap_usedSize);
    oh2_objc_register_instance_method("MTLHeap",           "maxAvailableSizeWithAlignment:",   (void *)&mtl_heap_maxAvail);
    oh2_objc_register_instance_method("MTLHeap",           "setPurgeableState:",               (void *)&mtl_cap_false1);
    oh2_objc_register_instance_method("MTLHeap",           "newBufferWithLength:options:",     (void *)&mtl_heap_newBuf);
    oh2_objc_register_instance_method("MTLHeap",           "newTextureWithDescriptor:",        (void *)&mtl_heap_newTex);

    oh2_objc_ensure_class("MTLCompileOptions", "NSObject");
    oh2_objc_register_instance_method("MTLCompileOptions", "init",                 (void *)&mtl_obj_ret_self);
    oh2_objc_register_instance_method("MTLCompileOptions", "fastMathEnabled",      (void *)&mtl_obj_one_u8);
    oh2_objc_register_instance_method("MTLCompileOptions", "setFastMathEnabled:",  (void *)&mtl_obj_noop1_u8);
    oh2_objc_register_instance_method("MTLCompileOptions", "languageVersion",      (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLCompileOptions", "setLanguageVersion:",  (void *)&mtl_obj_noop1_u64);
    oh2_objc_register_instance_method("MTLCompileOptions", "preprocessorMacros",   (void *)&mtl_obj_zero_ptr);
    oh2_objc_register_instance_method("MTLCompileOptions", "setPreprocessorMacros:", (void *)&mtl_cb_setLabel);

    oh2_objc_ensure_class("MTLFunctionConstantValues", "NSObject");
    oh2_objc_register_instance_method("MTLFunctionConstantValues", "init",                         (void *)&mtl_obj_ret_self);
    oh2_objc_register_instance_method("MTLFunctionConstantValues", "setConstantValue:type:atIndex:", (void *)&mtl_obj_noop3_const);
    oh2_objc_register_instance_method("MTLFunctionConstantValues", "setConstantValue:type:withName:", (void *)&mtl_obj_noop3_constName);
    oh2_objc_register_instance_method("MTLFunctionConstantValues", "setConstantValues:type:withRange:", (void *)&mtl_obj_noop3_constRange);
    oh2_objc_register_instance_method("MTLFunctionConstantValues", "reset",                        (void *)&mtl_cb_noop);

    oh2_objc_ensure_class("MTLPipelineBufferDescriptor", "NSObject");
    oh2_objc_ensure_class("MTLPipelineBufferDescriptorArray", "NSObject");
    oh2_objc_register_instance_method("MTLPipelineBufferDescriptorArray", "objectAtIndexedSubscript:", (void *)&mtl_pb_arr_subscript);
    oh2_objc_register_instance_method("MTLPipelineBufferDescriptor", "mutability",     (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLPipelineBufferDescriptor", "setMutability:", (void *)&mtl_obj_noop1_u64);

    oh2_objc_ensure_class("MTLArgumentDescriptor", "NSObject");
    oh2_objc_register_class_method("MTLArgumentDescriptor", "argumentDescriptor",        (void *)&mtl_cap_newScopeWithDev);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "dataType",               (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "setDataType:",           (void *)&mtl_obj_noop1_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "index",                  (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "setIndex:",              (void *)&mtl_obj_noop1_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "access",                 (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "setAccess:",             (void *)&mtl_obj_noop1_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "arrayLength",            (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "setArrayLength:",        (void *)&mtl_obj_noop1_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "textureType",            (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "setTextureType:",        (void *)&mtl_obj_noop1_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "constantBlockAlignment", (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLArgumentDescriptor", "setConstantBlockAlignment:", (void *)&mtl_obj_noop1_u64);

    oh2_objc_ensure_class("MTLTileRenderPipelineColorAttachmentDescriptor", "NSObject");
    oh2_objc_ensure_class("MTLTileRenderPipelineColorAttachmentDescriptorArray", "NSObject");
    static auto mtl_tile_ca_sub = +[](void *s, const char *sel, uint64_t idx) -> void * {
        (void)s; (void)sel; (void)idx;
        return oh2_objc_alloc_instance(oh2_objc_get_class("MTLTileRenderPipelineColorAttachmentDescriptor"), 64);
    };
    static auto mtl_tile_color_att = +[](void *s, const char *sel) -> void * {
        (void)s; (void)sel;
        return oh2_objc_alloc_instance(oh2_objc_get_class("MTLTileRenderPipelineColorAttachmentDescriptorArray"), 64);
    };
    oh2_objc_register_instance_method("MTLTileRenderPipelineColorAttachmentDescriptorArray", "objectAtIndexedSubscript:", (void *)mtl_tile_ca_sub);
    oh2_objc_register_instance_method("MTLTileRenderPipelineColorAttachmentDescriptor", "pixelFormat", (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLTileRenderPipelineColorAttachmentDescriptor", "setPixelFormat:", (void *)&mtl_obj_noop1_u64);

    oh2_objc_ensure_class("MTLTileRenderPipelineDescriptor", "NSObject");
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "init",           (void *)&mtl_obj_ret_self);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "label",          (void *)&mtl_cb_label);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "setLabel:",      (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "tileFunction",   (void *)&mtl_obj_zero_ptr);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "setTileFunction:", (void *)&mtl_cb_setLabel);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "rasterSampleCount", (void *)&mtl_obj_zero_u64);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "setRasterSampleCount:", (void *)&mtl_obj_noop1_u64);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "threadgroupSizeMatchesTileSize", (void *)&mtl_device_false);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "setThreadgroupSizeMatchesTileSize:", (void *)&mtl_obj_noop1_u8);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "tileBuffers",    (void *)&mtl_pb_arr_subscript);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "colorAttachments", (void *)mtl_tile_color_att);
    oh2_objc_register_instance_method("MTLTileRenderPipelineDescriptor", "reset",          (void *)&mtl_cb_noop);

    oh2_symbol_register_override("_MTLCreateSystemDefaultDevice", (void *)&mtl_create_system_default_device);
}

} // namespace oh2::mtlvk

/* ============================================================================
 * Public C API Implementation (mtl_vk.h)
 * ============================================================================ */
extern "C" {

int mtl_vk_init(ANativeWindow *window, const char *spirv_cache_path, int enable_validation) {
    using namespace oh2::mtlvk;
    MtlVkState &st = get_state();
    if (st.initialized) return 0;

    oh2::vk::VkContextConfig cfg{};
    cfg.window = window;
    cfg.enable_validation = (enable_validation != 0);
    cfg.app_name = "Oceanhorn2_Android";

    st.validation_enabled = (enable_validation != 0);
    if (!st.vk_ctx.init(cfg)) {
        MTLVK_LOGE("VkContext::init failed (validation=%d)", enable_validation);
        return -1;
    }
    if (!st.staging.init(st.vk_ctx.allocator(), st.vk_ctx.device(), st.vk_ctx.caps(),
                         oh2::vk::StagingRingBuffer::kMinCapacityBytes)) {
        MTLVK_LOGE("StagingRingBuffer::init failed");
        return -1;
    }

    VkDevice dev = st.vk_ctx.device();
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(dev, &pci, nullptr, &st.pipeline_cache);

    VkCommandPoolCreateInfo up_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    up_pool.queueFamilyIndex = st.vk_ctx.graphics_queue_family();
    up_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(dev, &up_pool, nullptr, &st.upload_pool) != VK_SUCCESS) return -1;
    VkFenceCreateInfo uf_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev, &uf_info, nullptr, &st.upload_fence) != VK_SUCCESS) return -1;

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        FrameSyncSlot &f = st.frames[i];
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = st.vk_ctx.graphics_queue_family();
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(dev, &cpi, nullptr, &f.cmd_pool) != VK_SUCCESS) return -1;

        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = f.cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &cbai, &f.cmd) != VK_SUCCESS) return -1;

        f.desc_pool = create_frame_descriptor_pool(dev);
        if (f.desc_pool == VK_NULL_HANDLE) return -1;
        f.overflow_desc_pools.clear();
        f.active_desc_pool_idx = 0;

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(dev, &si, nullptr, &f.image_available_sem) != VK_SUCCESS) return -1;

        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fi, nullptr, &f.in_flight_fence) != VK_SUCCESS) return -1;
        f.submitted = false;

        f.inline_capacity = 16ULL * 1024ULL * 1024ULL;
        VkBufferCreateInfo ibci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ibci.size = f.inline_capacity;
        ibci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        ibci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ivai{};
        ivai.usage = VMA_MEMORY_USAGE_AUTO;
        ivai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo ivinfo{};
        if (vmaCreateBuffer(st.vk_ctx.allocator(), &ibci, &ivai, &f.inline_buf, &f.inline_alloc, &ivinfo) != VK_SUCCESS) {
            return -1;
        }
        f.inline_mapped = static_cast<uint8_t *>(ivinfo.pMappedData);
        f.inline_cursor = 0;
    }

    for (uint32_t i = 0; i < 8; ++i) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(dev, &si, nullptr, &st.acquire_sem_pool[i]) != VK_SUCCESS) return -1;
    }

    if (!create_per_swapchain_sync(st)) return -1;
    st.initialized = true;
    if (!create_dummy_resources(st)) {
        st.initialized = false;
        return -1;
    }

    if (spirv_cache_path && spirv_cache_path[0]) {
        bool cache_ok = spirv_cache_load(spirv_cache_path);
        MTLVK_LOGI("SPIR-V cache load('%s'): ok=%d entries=%u",
                   spirv_cache_path, cache_ok ? 1 : 0, spirv_cache_count());
    }

    VkExtent2D ext = st.vk_ctx.swapchain_extent();
    oh2_ui_set_screen_dimensions(ext.width, ext.height, 2.0);
    MTLVK_LOGI("[M5] mtl_vk_init ready: GPU=%s swapchain=%ux%u dyn_render=%d astc=(%d,%d,%d) val=%d",
               st.vk_ctx.caps().device_name, ext.width, ext.height,
               st.vk_ctx.caps().dynamic_rendering ? 1 : 0,
               st.vk_ctx.caps().astc_4x4 ? 1 : 0,
               st.vk_ctx.caps().astc_6x6 ? 1 : 0,
               st.vk_ctx.caps().astc_8x8 ? 1 : 0,
               st.vk_ctx.caps().validation_layer_enabled ? 1 : 0);
    return 0;
}

int mtl_vk_set_surface(ANativeWindow *window) {
    using namespace oh2::mtlvk;
    MtlVkState &st = get_state();
    if (!st.initialized || !window) return -1;
    VkDevice dev = st.vk_ctx.device();
    vkDeviceWaitIdle(dev);
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (st.frames[i].submitted) {
            vkWaitForFences(dev, 1, &st.frames[i].in_flight_fence, VK_TRUE, UINT64_MAX);
            st.staging.notify_fence_completed(st.frames[i].in_flight_fence);
            st.frames[i].submitted = false;
        }
    }
    tick_deferred_destroys(true);
    if (!st.vk_ctx.rebind_surface(window)) {
        MTLVK_LOGE("VkContext::rebind_surface failed");
        return -1;
    }
    if (!create_per_swapchain_sync(st)) return -1;
    st.swapchain_out_of_date = false;
    st.surface_recreates.fetch_add(1, std::memory_order_relaxed);
    VkExtent2D ext = st.vk_ctx.swapchain_extent();
    oh2_ui_set_screen_dimensions(ext.width, ext.height, 2.0);
    if (st.shared_layer_obj) {
        auto *l = unwrap_mtl<MtlMetalLayerImpl>(st.shared_layer_obj);
        if (l) {
            l->drawable_width = (double)ext.width;
            l->drawable_height = (double)ext.height;
            l->has_active_drawable = false;
        }
    }
    MTLVK_LOGI("[M5] Swapchain surface rebuilt #%llu: %ux%u",
               (unsigned long long)st.surface_recreates.load(), ext.width, ext.height);
    return 0;
}

void mtl_vk_shutdown(void) {
    using namespace oh2::mtlvk;
    MtlVkState &st = get_state();
    if (!st.initialized) return;
    VkDevice dev = st.vk_ctx.device();
    vkDeviceWaitIdle(dev);
    tick_deferred_destroys(true);

    for (auto &kv : st.shader_modules) {
        if (kv.second != VK_NULL_HANDLE) vkDestroyShaderModule(dev, kv.second, nullptr);
    }
    st.shader_modules.clear();
    for (auto &kv : st.render_pass_cache) {
        if (kv.second != VK_NULL_HANDLE) vkDestroyRenderPass(dev, kv.second, nullptr);
    }
    st.render_pass_cache.clear();
    st.compat_render_pass_cache.clear();

    if (st.dummy_image_view != VK_NULL_HANDLE) vkDestroyImageView(dev, st.dummy_image_view, nullptr);
    if (st.dummy_image != VK_NULL_HANDLE) vmaDestroyImage(st.vk_ctx.allocator(), st.dummy_image, st.dummy_image_alloc);
    if (st.dummy_depth_view != VK_NULL_HANDLE) vkDestroyImageView(dev, st.dummy_depth_view, nullptr);
    if (st.dummy_depth_image != VK_NULL_HANDLE) vmaDestroyImage(st.vk_ctx.allocator(), st.dummy_depth_image, st.dummy_depth_alloc);
    if (st.dummy_sampler != VK_NULL_HANDLE) vkDestroySampler(dev, st.dummy_sampler, nullptr);
    if (st.dummy_buffer != VK_NULL_HANDLE) vmaDestroyBuffer(st.vk_ctx.allocator(), st.dummy_buffer, st.dummy_buffer_alloc);
    if (st.fb_fetch_view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, st.fb_fetch_view, nullptr);
        st.fb_fetch_view = VK_NULL_HANDLE;
    }
    if (st.fb_fetch_image != VK_NULL_HANDLE) {
        vmaDestroyImage(st.vk_ctx.allocator(), st.fb_fetch_image, st.fb_fetch_alloc);
        st.fb_fetch_image = VK_NULL_HANDLE;
        st.fb_fetch_alloc = VK_NULL_HANDLE;
    }
    st.fb_fetch_w = 0;
    st.fb_fetch_h = 0;
    st.fb_fetch_fmt = VK_FORMAT_UNDEFINED;

    for (VkSemaphore s : st.render_finished_sems) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(dev, s, nullptr);
    }
    st.render_finished_sems.clear();

    for (uint32_t i = 0; i < 8; ++i) {
        if (st.acquire_sem_pool[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, st.acquire_sem_pool[i], nullptr);
            st.acquire_sem_pool[i] = VK_NULL_HANDLE;
        }
    }

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (st.frames[i].inline_buf != VK_NULL_HANDLE) {
            vmaDestroyBuffer(st.vk_ctx.allocator(), st.frames[i].inline_buf, st.frames[i].inline_alloc);
            st.frames[i].inline_buf = VK_NULL_HANDLE;
            st.frames[i].inline_mapped = nullptr;
        }
        if (st.frames[i].image_available_sem != VK_NULL_HANDLE) vkDestroySemaphore(dev, st.frames[i].image_available_sem, nullptr);
        if (st.frames[i].in_flight_fence != VK_NULL_HANDLE) vkDestroyFence(dev, st.frames[i].in_flight_fence, nullptr);
        if (st.frames[i].desc_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, st.frames[i].desc_pool, nullptr);
        for (VkDescriptorPool p : st.frames[i].overflow_desc_pools) {
            if (p != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, p, nullptr);
        }
        st.frames[i].overflow_desc_pools.clear();
        if (st.frames[i].cmd_pool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, st.frames[i].cmd_pool, nullptr);
    }

    if (st.upload_fence != VK_NULL_HANDLE) vkDestroyFence(dev, st.upload_fence, nullptr);
    if (st.upload_pool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, st.upload_pool, nullptr);
    if (st.pipeline_cache != VK_NULL_HANDLE) vkDestroyPipelineCache(dev, st.pipeline_cache, nullptr);

    st.staging.destroy();
    st.vk_ctx.destroy();
    spirv_cache_unload();
    st.initialized = false;
}

void mtl_vk_register_objc_bridges(void) {
    using namespace oh2::mtlvk;
    register_device_objc_methods();
    register_resource_objc_methods();
    register_pipeline_objc_methods();
    register_encoder_objc_methods();
    MTLVK_LOGI("[M5] Registered Metal-to-Vulkan ObjC bridges + CAMetalLayer");
}

void mtl_vk_get_stats(MtlVkStats *out_stats) {
    using namespace oh2::mtlvk;
    if (!out_stats) return;
    MtlVkState &st = get_state();
    out_stats->draw_calls = st.draw_calls.load(std::memory_order_relaxed);
    out_stats->indexed_draw_calls = st.indexed_draw_calls.load(std::memory_order_relaxed);
    out_stats->compute_dispatches = st.compute_dispatches.load(std::memory_order_relaxed);
    out_stats->blit_commands = st.blit_commands.load(std::memory_order_relaxed);
    out_stats->command_buffers_committed = st.command_buffers_committed.load(std::memory_order_relaxed);
    out_stats->presents = st.presents.load(std::memory_order_relaxed);
    out_stats->graphics_pipelines_created = st.graphics_pipelines_created.load(std::memory_order_relaxed);
    out_stats->compute_pipelines_created = st.compute_pipelines_created.load(std::memory_order_relaxed);
    out_stats->astc_textures_created = st.astc_textures_created.load(std::memory_order_relaxed);
    out_stats->astc_cpu_decompress_count = st.astc_cpu_decompress_count.load(std::memory_order_relaxed);
    out_stats->surface_recreates = st.surface_recreates.load(std::memory_order_relaxed);
    out_stats->validation_errors = st.vk_ctx.validation_errors();
    out_stats->validation_warnings = st.vk_ctx.validation_warnings();
    out_stats->spirv_cache_entries = spirv_cache_count();
    out_stats->dynamic_rendering_active = st.vk_ctx.caps().dynamic_rendering ? 1u : 0u;
}

} // extern "C"
