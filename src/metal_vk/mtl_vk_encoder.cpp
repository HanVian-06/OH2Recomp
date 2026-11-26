#include "mtl_vk_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace oh2::mtlvk {

namespace {

static bool check_begin_cb(MtlCommandBufferImpl *cb) {
    return cb && (cb->recording || mtl_begin_command_buffer_internal(cb) != VK_NULL_HANDLE);
}

static inline void mix_desc_hash(uint64_t &h1, uint64_t &h2, uint64_t a, uint64_t b) {
    h1 ^= a + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2);
    h2 ^= b + 0xbf58476d1ce4e5b9ULL + (h2 << 7) + (h2 >> 3);
    h1 *= 1099511628211ULL;
    h2 *= 0x94d049bb133111ebULL;
}

static VkDescriptorSet allocate_and_write_set(MtlCommandBufferImpl *cb,
                                              VkDescriptorSetLayout layout,
                                              const SpirvShaderEntry *entry,
                                              const MtlStageBindings &bindings,
                                              const uint32_t *smp_tex_mask = nullptr,
                                              uint32_t dyn_ubo_mask = 0,
                                              const uint8_t *dyn_slots = nullptr,
                                              uint8_t dyn_cnt = 0,
                                              uint32_t *out_dyn_offsets = nullptr) {
    MtlVkState &st = get_state();
    if (!cb || layout == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    FrameSyncSlot &slot = st.frames[cb->frame_idx];

    const uint32_t bound_depth_tex_mask = bindings.depth_tex_mask;
    const uint16_t bcnt = (entry && entry->bindings) ? std::min<uint16_t>(entry->binding_count, 64) : 0;

    if (out_dyn_offsets && dyn_cnt > 0) {
        for (uint8_t d = 0; d < dyn_cnt && d < 8; ++d) out_dyn_offsets[d] = 0;
    }

    uint64_t h1 = 14695981039346656037ULL ^ (uint64_t)(uintptr_t)layout ^ ((uint64_t)bcnt << 48);
    uint64_t h2 = 0xcbf29ce484222325ULL ^ ((uint64_t)(uintptr_t)layout * 0x9e3779b97f4a7c15ULL) ^ bcnt;

    /* Pass 1 (Hot Fast Path — >99% cache hit): compute descriptor hash + dynamic UBO offsets with zero stack write churn */
    VkBuffer     resolved_buf[64];
    VkDeviceSize resolved_off[64];
    VkDeviceSize resolved_rng[64];
    bool         resolved_dyn[64];

    for (uint16_t i = 0; i < bcnt; ++i) {
        const SpirvBindingDesc &b = entry->bindings[i];
        uint8_t ms = b.metal_slot;

        if (b.descriptor_type == SPIRV_VK_DESC_UNIFORM_BUFFER ||
            b.descriptor_type == SPIRV_VK_DESC_STORAGE_BUFFER) {
            const bool is_dyn_ubo = (b.descriptor_type == SPIRV_VK_DESC_UNIFORM_BUFFER) &&
                                    (ms < kMaxBuffers) && ((dyn_ubo_mask & (1u << ms)) != 0);
            VkBuffer vbuf = st.dummy_buffer;
            uint64_t buid = 1ULL;
            VkDeviceSize voff = 0;
            VkDeviceSize vrange = is_dyn_ubo ? 4096ULL : VK_WHOLE_SIZE;
            if (ms < kMaxBuffers && bindings.buffer_bound[ms] && bindings.buffer[ms] != VK_NULL_HANDLE) {
                vbuf = bindings.buffer[ms];
                buid = bindings.buffer_uid[ms] ? bindings.buffer_uid[ms] : (uint64_t)(uintptr_t)vbuf;
                voff = bindings.buffer_offset[ms];
                uint64_t total_len = bindings.buffer_total_len[ms];
                uint64_t avail = (total_len > voff) ? (total_len - voff) : 0ULL;
                const uint64_t max_ubo = 65536ULL;
                if (is_dyn_ubo) {
                    if (__builtin_expect(st.min_ubo_offset_align > 1 && (voff % st.min_ubo_offset_align) != 0 &&
                                         bindings.buffer_mapped[ms] != nullptr && avail > 0, 0)) {
                        uint64_t copy_len = std::min<uint64_t>(avail, max_ubo);
                        VkBuffer tmp_buf = VK_NULL_HANDLE;
                        uint64_t tmp_off = 0;
                        if (mtl_alloc_inline_bytes(cb, bindings.buffer_mapped[ms] + voff, copy_len, &tmp_buf, &tmp_off)) {
                            vbuf = tmp_buf;
                            buid = (uint64_t)(uintptr_t)tmp_buf;
                            voff = tmp_off;
                            total_len = slot.inline_capacity;
                            avail = copy_len;
                        }
                    }
                    if (vbuf == slot.inline_buf) {
                        uint64_t req = (bindings.buffer_range[ms] > 0) ? bindings.buffer_range[ms] : avail;
                        vrange = std::min<VkDeviceSize>(std::max<VkDeviceSize>(16ULL, req), max_ubo);
                        if (voff + vrange > slot.inline_capacity) {
                            vrange = (slot.inline_capacity > voff) ? (slot.inline_capacity - voff) : 16ULL;
                        }
                    } else {
                        if (avail >= max_ubo)      vrange = max_ubo;
                        else if (avail >= 16384ULL) vrange = 16384ULL;
                        else if (avail >= 4096ULL)  vrange = 4096ULL;
                        else                        vrange = std::max<VkDeviceSize>(16ULL, avail);
                    }
                } else {
                    vrange = (bindings.buffer_range[ms] > 0) ? bindings.buffer_range[ms] : VK_WHOLE_SIZE;
                    if (b.descriptor_type == SPIRV_VK_DESC_UNIFORM_BUFFER) {
                        if (vrange == VK_WHOLE_SIZE || vrange > max_ubo) {
                            vrange = (avail > 0) ? std::min<VkDeviceSize>(avail, max_ubo) : max_ubo;
                        }
                    }
                }
            }
            resolved_buf[i] = vbuf;
            resolved_off[i] = voff;
            resolved_rng[i] = vrange;
            resolved_dyn[i] = is_dyn_ubo;
            if (is_dyn_ubo) {
                if (out_dyn_offsets && dyn_slots) {
                    for (uint8_t d = 0; d < dyn_cnt && d < 8; ++d) {
                        if (dyn_slots[d] == ms) {
                            out_dyn_offsets[d] = static_cast<uint32_t>(voff);
                            break;
                        }
                    }
                }
                mix_desc_hash(h1, h2, buid ^ ((uint64_t)b.binding << 56), vrange ^ (1ULL << 62));
            } else {
                mix_desc_hash(h1, h2, buid ^ ((uint64_t)b.binding << 56), voff ^ (vrange << 17));
            }
        } else if (b.descriptor_type == SPIRV_VK_DESC_SAMPLER) {
            VkSampler vsmp = st.dummy_sampler;
            if (ms < kMaxSamplers && bindings.sampler_bound[ms] && bindings.sampler[ms] != VK_NULL_HANDLE) {
                const bool samples_depth = (smp_tex_mask != nullptr) && ((smp_tex_mask[ms] & bound_depth_tex_mask) != 0);
                vsmp = (samples_depth && bindings.sampler_nearest[ms] != VK_NULL_HANDLE)
                           ? bindings.sampler_nearest[ms]
                           : bindings.sampler[ms];
            }
            mix_desc_hash(h1, h2, (uint64_t)(uintptr_t)vsmp, (uint64_t)b.binding | (2ULL << 32));
        } else {
            VkImageView vview = b.is_depth ? st.dummy_depth_view : st.dummy_image_view;
            uint64_t tuid = b.is_depth ? 2ULL : 3ULL;
            if (ms < kMaxTextures && bindings.tex_bound[ms] && bindings.tex_view[ms] != VK_NULL_HANDLE) {
                vview = bindings.tex_view[ms];
                tuid = bindings.tex_uid[ms] ? bindings.tex_uid[ms] : (uint64_t)(uintptr_t)vview;
            }
            mix_desc_hash(h1, h2, tuid, (uint64_t)b.binding | (3ULL << 32));
        }
    }

    DescCacheEntry &dce = slot.desc_cache[h1 & (kDescCacheSize - 1u)];
    if (__builtin_expect(dce.epoch == slot.desc_cache_epoch &&
                         dce.layout == layout &&
                         dce.h1 == h1 &&
                         dce.h2 == h2 &&
                         dce.set != VK_NULL_HANDLE, 1)) {
        st.prof_desc_hits.fetch_add(1, std::memory_order_relaxed);
        return dce.set;
    }
    st.prof_desc_misses.fetch_add(1, std::memory_order_relaxed);

    /* Pass 2 (Cold Miss Path): allocate VkDescriptorSet & write descriptors */
    VkDescriptorPool pool = (slot.active_desc_pool_idx == 0)
                                ? slot.desc_pool
                                : slot.overflow_desc_pools[slot.active_desc_pool_idx - 1];

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout;

    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkResult ares = vkAllocateDescriptorSets(st.vk_ctx.device(), &dsai, &dset);
    if (ares == VK_ERROR_OUT_OF_POOL_MEMORY || ares == VK_ERROR_FRAGMENTED_POOL) {
        slot.active_desc_pool_idx++;
        if (slot.active_desc_pool_idx - 1 >= slot.overflow_desc_pools.size()) {
            VkDescriptorPool np = create_frame_descriptor_pool(st.vk_ctx.device());
            if (np == VK_NULL_HANDLE) {
                slot.active_desc_pool_idx--;
                MTLVK_LOGE("vkAllocateDescriptorSets overflow pool creation failed");
                return VK_NULL_HANDLE;
            }
            slot.overflow_desc_pools.push_back(np);
        }
        dsai.descriptorPool = slot.overflow_desc_pools[slot.active_desc_pool_idx - 1];
        ares = vkAllocateDescriptorSets(st.vk_ctx.device(), &dsai, &dset);
    }
    if (ares != VK_SUCCESS) {
        MTLVK_LOGE("vkAllocateDescriptorSets failed (%d)", static_cast<int>(ares));
        return VK_NULL_HANDLE;
    }
    slot.desc_sets_allocated++;

    if (bcnt > 0) {
        VkDescriptorBufferInfo buf_infos[64];
        VkDescriptorImageInfo img_infos[64];
        VkWriteDescriptorSet writes[64];
        for (uint16_t i = 0; i < bcnt; ++i) {
            const SpirvBindingDesc &b = entry->bindings[i];
            uint8_t ms = b.metal_slot;
            VkWriteDescriptorSet &w = writes[i];
            w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = dset;
            w.dstBinding = b.binding;
            w.dstArrayElement = 0;
            w.descriptorCount = 1;
            if (b.descriptor_type == SPIRV_VK_DESC_UNIFORM_BUFFER ||
                b.descriptor_type == SPIRV_VK_DESC_STORAGE_BUFFER) {
                w.descriptorType = (b.descriptor_type == SPIRV_VK_DESC_STORAGE_BUFFER)
                                       ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                       : (resolved_dyn[i] ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                          : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                buf_infos[i] = {resolved_buf[i], resolved_dyn[i] ? 0 : resolved_off[i], resolved_rng[i]};
                w.pBufferInfo = &buf_infos[i];
                if (ms < kMaxBuffers && bindings.buffer_obj[ms]) {
                    if (auto *mb = unwrap_mtl<MtlBufferImpl>(bindings.buffer_obj[ms])) mb->bound_as_descriptor = true;
                }
            } else if (b.descriptor_type == SPIRV_VK_DESC_SAMPLER) {
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                VkSampler vsmp = st.dummy_sampler;
                if (ms < kMaxSamplers && bindings.sampler_bound[ms] && bindings.sampler[ms] != VK_NULL_HANDLE) {
                    const bool samples_depth = (smp_tex_mask != nullptr) && ((smp_tex_mask[ms] & bound_depth_tex_mask) != 0);
                    vsmp = (samples_depth && bindings.sampler_nearest[ms] != VK_NULL_HANDLE)
                               ? bindings.sampler_nearest[ms]
                               : bindings.sampler[ms];
                }
                img_infos[i] = {vsmp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                w.pImageInfo = &img_infos[i];
            } else {
                w.descriptorType = (b.descriptor_type == SPIRV_VK_DESC_STORAGE_IMAGE)
                                       ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                       : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                VkImageView vview = b.is_depth ? st.dummy_depth_view : st.dummy_image_view;
                if (ms < kMaxTextures && bindings.tex_bound[ms] && bindings.tex_view[ms] != VK_NULL_HANDLE) {
                    vview = bindings.tex_view[ms];
                    if (bindings.tex_obj[ms]) {
                        if (auto *mt = unwrap_mtl<MtlTextureImpl>(bindings.tex_obj[ms])) mt->bound_as_descriptor = true;
                    }
                }
                img_infos[i] = {
                    VK_NULL_HANDLE,
                    vview,
                    (b.descriptor_type == SPIRV_VK_DESC_STORAGE_IMAGE)
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                };
                w.pImageInfo = &img_infos[i];
            }
        }
        vkUpdateDescriptorSets(st.vk_ctx.device(), static_cast<uint32_t>(bcnt), writes, 0, nullptr);
    }

    dce.layout = layout;
    dce.set = dset;
    dce.h1 = h1;
    dce.h2 = h2;
    dce.epoch = slot.desc_cache_epoch;
    return dset;
}

/* ============================================================================
 * MTLRenderCommandEncoder
 * ============================================================================ */
static bool begin_render_pass_internal(MtlRenderEncoderImpl *re) {
    MtlVkState &st = get_state();
    if (!re || !check_begin_cb(re->cmd_buf)) return false;
    if (re->inside_render_pass) return true;

    VkCommandBuffer cmd = re->cmd_buf->cmd;
    uint32_t w = re->pass_desc.rt_width;
    uint32_t h = re->pass_desc.rt_height;

    std::vector<VkRenderingAttachmentInfoKHR> dyn_colors;
    std::vector<VkClearValue> fb_clears;
    std::vector<VkImageView> fb_views;
    uint64_t color_loads[kMaxColorAttachments]{};
    uint64_t color_stores[kMaxColorAttachments]{};

    re->color_count = 0;
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        const auto &ca = re->pass_desc.color[i];
        if (!ca.texture_obj) continue;
        auto *tex = unwrap_mtl<MtlTextureImpl>(ca.texture_obj);
        if (!tex || tex->image == VK_NULL_HANDLE) continue;

        uint32_t lvl = (uint32_t)ca.level;
        uint32_t slc = (uint32_t)ca.slice;
        if (w == 0) w = std::max(1u, tex->width >> lvl);
        if (h == 0) h = std::max(1u, tex->height >> lvl);

        VkImageView att_view = tex->view;
        if (lvl != 0 || slc != 0 || tex->mip_levels > 1 || tex->array_layers > 1) {
            VkImageView cached_v = VK_NULL_HANDLE;
            for (const auto &sv : tex->subres_att_views) {
                if (sv.level == lvl && sv.slice == slc) {
                    cached_v = sv.view;
                    break;
                }
            }
            if (cached_v != VK_NULL_HANDLE) {
                att_view = cached_v;
            } else {
                VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                ivci.image = tex->image;
                ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                ivci.format = tex->vk_format;
                ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, 1, slc, 1};
                if (vkCreateImageView(st.vk_ctx.device(), &ivci, nullptr, &att_view) == VK_SUCCESS) {
                    tex->subres_att_views.push_back({static_cast<uint16_t>(lvl), static_cast<uint16_t>(slc), att_view});
                } else {
                    att_view = tex->view;
                }
            }
        }

        VkImageLayout old_l = tex->get_subres_layout(lvl, slc);
        VkImageLayout req_old = (ca.load_action == kMTLLoadActionLoad) ? old_l : VK_IMAGE_LAYOUT_UNDEFINED;
        transition_image_layout(cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, lvl, 1, slc, 1,
                                req_old, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        tex->set_subres_layout(lvl, slc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfoKHR rai{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
        rai.imageView = att_view;
        rai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        rai.loadOp = (ca.load_action == kMTLLoadActionClear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                     (ca.load_action == kMTLLoadActionLoad)  ? VK_ATTACHMENT_LOAD_OP_LOAD :
                                                               VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        /* Always use STORE_OP_STORE so mid-pass suspends (fb_fetch / buffer-texture sync) and late
           setColorStoreAction:atIndex: calls never discard render targets in tile memory. */
        rai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        rai.clearValue.color.float32[0] = (float)ca.clear_color.red;
        rai.clearValue.color.float32[1] = (float)ca.clear_color.green;
        rai.clearValue.color.float32[2] = (float)ca.clear_color.blue;
        rai.clearValue.color.float32[3] = (float)ca.clear_color.alpha;

        dyn_colors.push_back(rai);
        fb_views.push_back(att_view);
        fb_clears.push_back(rai.clearValue);

        re->color_formats[re->color_count] = tex->vk_format;
        color_loads[re->color_count] = ca.load_action;
        color_stores[re->color_count] = ca.store_action;
        re->color_count++;
    }

    VkRenderingAttachmentInfoKHR dyn_depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingAttachmentInfoKHR dyn_stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    bool has_depth = false;
    bool has_stencil = false;
    re->depth_format = VK_FORMAT_UNDEFINED;
    re->stencil_format = VK_FORMAT_UNDEFINED;

    if (re->pass_desc.depth.texture_obj) {
        auto *dtex = unwrap_mtl<MtlTextureImpl>(re->pass_desc.depth.texture_obj);
        if (dtex && dtex->image != VK_NULL_HANDLE) {
            uint32_t lvl = (uint32_t)re->pass_desc.depth.level;
            uint32_t slc = (uint32_t)re->pass_desc.depth.slice;
            if (w == 0) w = std::max(1u, dtex->width >> lvl);
            if (h == 0) h = std::max(1u, dtex->height >> lvl);

            VkImageLayout old_l = dtex->get_subres_layout(lvl, slc);
            VkImageLayout req_old = (re->pass_desc.depth.load_action == kMTLLoadActionLoad) ? old_l : VK_IMAGE_LAYOUT_UNDEFINED;
            transition_image_layout(cmd, dtex->image, dtex->aspect_mask, lvl, 1, slc, 1,
                                    req_old, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            dtex->set_subres_layout(lvl, slc, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

            VkImageView dview = dtex->view;
            if ((dtex->aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
                if (lvl == 0 && slc == 0) {
                    if (dtex->ds_att_view == VK_NULL_HANDLE) {
                        VkImageViewCreateInfo div{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                        div.image = dtex->image;
                        div.viewType = VK_IMAGE_VIEW_TYPE_2D;
                        div.format = dtex->vk_format;
                        div.subresourceRange = {dtex->aspect_mask, 0, 1, 0, 1};
                        vkCreateImageView(st.vk_ctx.device(), &div, nullptr, &dtex->ds_att_view);
                    }
                    if (dtex->ds_att_view != VK_NULL_HANDLE) dview = dtex->ds_att_view;
                } else {
                    VkImageView cached_dv = VK_NULL_HANDLE;
                    for (const auto &sv : dtex->subres_att_views) {
                        if (sv.level == lvl && sv.slice == slc) {
                            cached_dv = sv.view;
                            break;
                        }
                    }
                    if (cached_dv != VK_NULL_HANDLE) {
                        dview = cached_dv;
                    } else {
                        VkImageViewCreateInfo div{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                        div.image = dtex->image;
                        div.viewType = VK_IMAGE_VIEW_TYPE_2D;
                        div.format = dtex->vk_format;
                        div.subresourceRange = {dtex->aspect_mask, lvl, 1, slc, 1};
                        if (vkCreateImageView(st.vk_ctx.device(), &div, nullptr, &dview) == VK_SUCCESS) {
                            dtex->subres_att_views.push_back({static_cast<uint16_t>(lvl), static_cast<uint16_t>(slc), dview});
                        }
                    }
                }
            }

            has_depth = true;
            re->depth_format = dtex->vk_format;
            if (dtex->aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) {
                has_stencil = true;
                re->stencil_format = dtex->vk_format;
            }

            dyn_depth.imageView = dview;
            dyn_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            dyn_depth.loadOp = (re->pass_desc.depth.load_action == kMTLLoadActionClear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                               (re->pass_desc.depth.load_action == kMTLLoadActionLoad)  ? VK_ATTACHMENT_LOAD_OP_LOAD :
                                                                                         VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            /* Always store depth/stencil so mid-pass suspends (fb_fetch / buffer-texture sync) and late
               setDepthStoreAction: calls never discard the 3D scene depth buffer! */
            dyn_depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            dyn_depth.clearValue.depthStencil = {(float)re->pass_desc.depth.clear_depth, re->pass_desc.stencil.clear_stencil};
            fb_views.push_back(dview);
            fb_clears.push_back(dyn_depth.clearValue);

            if (has_stencil) {
                dyn_stencil = dyn_depth;
            }
        }
    }

    if (w == 0) w = st.vk_ctx.swapchain_extent().width;
    if (h == 0) h = st.vk_ctx.swapchain_extent().height;
    re->render_w = std::max(1u, w);
    re->render_h = std::max(1u, h);

    if (st.vk_ctx.caps().dynamic_rendering && st.vk_ctx.cmd_begin_rendering()) {
        VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
        ri.renderArea = {{0, 0}, {re->render_w, re->render_h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = static_cast<uint32_t>(dyn_colors.size());
        ri.pColorAttachments = dyn_colors.empty() ? nullptr : dyn_colors.data();
        ri.pDepthAttachment = has_depth ? &dyn_depth : nullptr;
        ri.pStencilAttachment = has_stencil ? &dyn_stencil : nullptr;
        st.vk_ctx.cmd_begin_rendering()(cmd, &ri);
    } else {
        re->fallback_render_pass = get_or_create_render_pass(
            re->color_formats, re->color_count, color_loads, color_stores,
            re->depth_format, re->pass_desc.depth.load_action, re->pass_desc.depth.store_action, false);
        if (re->fallback_render_pass == VK_NULL_HANDLE) return false;

        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = re->fallback_render_pass;
        fbci.attachmentCount = static_cast<uint32_t>(fb_views.size());
        fbci.pAttachments = fb_views.data();
        fbci.width = re->render_w;
        fbci.height = re->render_h;
        fbci.layers = 1;
        if (vkCreateFramebuffer(st.vk_ctx.device(), &fbci, nullptr, &re->fallback_framebuffer) != VK_SUCCESS) {
            return false;
        }
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = re->fallback_render_pass;
        rpbi.framebuffer = re->fallback_framebuffer;
        rpbi.renderArea = {{0, 0}, {re->render_w, re->render_h}};
        rpbi.clearValueCount = static_cast<uint32_t>(fb_clears.size());
        rpbi.pClearValues = fb_clears.data();
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    }

    re->inside_render_pass = true;
    re->bound_pipeline = VK_NULL_HANDLE;
    re->bound_pso_obj = nullptr;
    re->bound_dss_obj = nullptr;
    re->bound_ptype = UINT64_MAX;
    re->bound_cull = UINT64_MAX;
    re->bound_wind = UINT64_MAX;
    re->bound_fill = UINT64_MAX;
    re->bound_set0 = VK_NULL_HANDLE;
    re->bound_set0_layout = VK_NULL_HANDLE;
    re->bound_set1 = VK_NULL_HANDLE;
    re->bound_set1_layout = VK_NULL_HANDLE;
    re->bound_pipe_layout = VK_NULL_HANDLE;
    re->bound_set0_dyn_count = 0;
    re->bound_set1_dyn_count = 0;
    re->viewport_dirty = true;
    re->scissor_dirty = true;
    re->depth_bias_dirty = true;
    re->blend_constants_dirty = true;
    re->stencil_ref_dirty = true;
    re->vs.desc_dirty = true;
    re->vs.dirty_buf_mask = 0xFFFFFFFFu;
    re->vs.dirty_tex_mask = 0xFFFFFFFFu;
    re->vs.dirty_smp_mask = 0xFFFFFFFFu;
    re->vs.vb_dirty_mask = 0xFFFFFFFFu;
    re->ps.desc_dirty = true;
    re->ps.dirty_buf_mask = 0xFFFFFFFFu;
    re->ps.dirty_tex_mask = 0xFFFFFFFFu;
    re->ps.dirty_smp_mask = 0xFFFFFFFFu;
    /* Set default negative-height viewport & full scissor immediately */
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = (float)re->render_h;
    vp.width = (float)re->render_w;
    vp.height = -(float)re->render_h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{{0, 0}, {re->render_w, re->render_h}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    return true;
}

static void snapshot_color0_for_fb_fetch(MtlRenderEncoderImpl *re) {
    MtlVkState &st = get_state();
    if (!re || !re->inside_render_pass || !re->pass_desc.color[0].texture_obj) return;
    auto *c0_tex = unwrap_mtl<MtlTextureImpl>(re->pass_desc.color[0].texture_obj);
    if (!c0_tex || c0_tex->image == VK_NULL_HANDLE) return;

    uint32_t lvl = static_cast<uint32_t>(re->pass_desc.color[0].level);
    uint32_t slc = static_cast<uint32_t>(re->pass_desc.color[0].slice);
    uint32_t w = std::max(1u, c0_tex->width >> lvl);
    uint32_t h = std::max(1u, c0_tex->height >> lvl);
    VkFormat fmt = c0_tex->vk_format;
    uint32_t occ = re->cmd_buf ? (re->cmd_buf->fb_snap_seq++ & 3u) : 0u;

    MtlVkState::FbFetchPoolEntry *fb_ent = nullptr;
    for (auto &e : st.fb_fetch_pool) {
        if (e.w == w && e.h == h && e.fmt == fmt && e.occ == occ && e.image != VK_NULL_HANDLE) {
            fb_ent = &e;
            break;
        }
    }
    if (!fb_ent) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo vai{};
        vai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VkImage new_img = VK_NULL_HANDLE;
        VmaAllocation new_alloc = VK_NULL_HANDLE;
        if (vmaCreateImage(st.vk_ctx.allocator(), &ici, &vai, &new_img, &new_alloc, nullptr) != VK_SUCCESS) {
            return;
        }
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = new_img;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = fmt;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView new_view = VK_NULL_HANDLE;
        if (vkCreateImageView(st.vk_ctx.device(), &ivci, nullptr, &new_view) != VK_SUCCESS) {
            vmaDestroyImage(st.vk_ctx.allocator(), new_img, new_alloc);
            return;
        }
        MtlVkState::FbFetchPoolEntry ne{};
        ne.w = w;
        ne.h = h;
        ne.fmt = fmt;
        ne.occ = occ;
        ne.image = new_img;
        ne.alloc = new_alloc;
        ne.view = new_view;
        ne.uid = next_mtl_resource_uid();
        ne.initialized = false;
        st.fb_fetch_pool.push_back(ne);
        fb_ent = &st.fb_fetch_pool.back();
    }

    VkCommandBuffer cmd = re->cmd_buf->cmd;
    if (re->occ_query_active && re->cmd_buf) {
        FrameSyncSlot &slot = st.frames[re->cmd_buf->frame_idx];
        if (slot.occ_query_pool != VK_NULL_HANDLE) {
            vkCmdEndQuery(cmd, slot.occ_query_pool, re->occ_active_qidx);
        }
        re->occ_query_active = false;
    }
    if (st.vk_ctx.caps().dynamic_rendering && st.vk_ctx.cmd_end_rendering()) {
        st.vk_ctx.cmd_end_rendering()(cmd);
    } else {
        vkCmdEndRenderPass(cmd);
    }
    re->inside_render_pass = false;
    for (VkImageView v : re->attachment_views_to_destroy) {
        enqueue_deferred_view(v);
    }
    re->attachment_views_to_destroy.clear();
    if (re->fallback_framebuffer != VK_NULL_HANDLE) {
        enqueue_deferred_framebuffer(re->fallback_framebuffer);
        re->fallback_framebuffer = VK_NULL_HANDLE;
    }

    transition_image_layout(cmd, c0_tex->image, VK_IMAGE_ASPECT_COLOR_BIT, lvl, 1, slc, 1,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition_image_layout(cmd, fb_ent->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                            fb_ent->initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy ic{};
    ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, slc, 1};
    ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    ic.extent = {w, h, 1};
    vkCmdCopyImage(cmd,
                   c0_tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   fb_ent->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &ic);

    transition_image_layout(cmd, fb_ent->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    fb_ent->initialized = true;
    transition_image_layout(cmd, c0_tex->image, VK_IMAGE_ASPECT_COLOR_BIT, lvl, 1, slc, 1,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    c0_tex->set_subres_layout(lvl, slc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkMemoryBarrier att_mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    att_mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    att_mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 1, &att_mb, 0, nullptr, 0, nullptr);

    uint64_t saved_color_loads[kMaxColorAttachments]{};
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        saved_color_loads[i] = re->pass_desc.color[i].load_action;
        re->pass_desc.color[i].load_action = kMTLLoadActionLoad;
    }
    uint64_t saved_depth_load = re->pass_desc.depth.load_action;
    uint64_t saved_stencil_load = re->pass_desc.stencil.load_action;
    re->pass_desc.depth.load_action = kMTLLoadActionLoad;
    re->pass_desc.stencil.load_action = kMTLLoadActionLoad;

    st.prof_render_passes.fetch_add(1, std::memory_order_relaxed);
    begin_render_pass_internal(re);

    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        re->pass_desc.color[i].load_action = saved_color_loads[i];
    }
    re->pass_desc.depth.load_action = saved_depth_load;
    re->pass_desc.stencil.load_action = saved_stencil_load;

    re->ps.tex_view[30] = fb_ent->view;
    re->ps.tex_uid[30] = fb_ent->uid;
    re->ps.tex_obj[30] = nullptr;
    re->ps.tex_is_depth[30] = false;
    re->ps.depth_tex_mask &= ~(1u << 30);
    re->ps.tex_bound[30] = true;
    re->ps.desc_dirty = true;
    re->ps.dirty_tex_mask |= (1u << 30);
}

static void record_buffer_backed_texture_copy(VkCommandBuffer cmd, MtlTextureImpl *tex, MtlBufferImpl *buf) {
    if (!tex || !buf || tex->image == VK_NULL_HANDLE || buf->buffer == VK_NULL_HANDLE) return;
    uint32_t bw = 1, bh = 1;
    uint32_t bsz = mtl_pixel_format_block_size(tex->vk_format, &bw, &bh);
    if (bsz == 0) bsz = 4;

    VkDeviceSize aligned_off = (bsz >= 4)
                                   ? static_cast<VkDeviceSize>(tex->buffer_offset & ~static_cast<uint64_t>(bsz - 1))
                                   : static_cast<VkDeviceSize>(tex->buffer_offset & ~3ULL);
    if (aligned_off >= buf->length) return;
    uint64_t avail = buf->length - aligned_off;

    uint32_t buf_row_len = 0;
    if (tex->buffer_bytes_per_row > 0) {
        uint32_t row_texels = static_cast<uint32_t>((tex->buffer_bytes_per_row / bsz) * bw);
        if (row_texels >= tex->width && row_texels <= 65536u) {
            buf_row_len = row_texels;
        }
    }
    uint64_t row_bytes = (buf_row_len > 0)
                             ? (static_cast<uint64_t>(buf_row_len / bw) * bsz)
                             : (static_cast<uint64_t>((tex->width + bw - 1u) / bw) * bsz);
    if (row_bytes == 0) return;

    uint32_t copy_w = tex->width;
    uint32_t copy_h = tex->height;
    if (tex->height > 1) {
        copy_h = static_cast<uint32_t>(std::min<uint64_t>(tex->height, avail / row_bytes));
        if (copy_h == 0 && avail >= bsz) {
            copy_h = 1;
            copy_w = std::min<uint32_t>(tex->width, static_cast<uint32_t>((avail / bsz) * bw));
        }
    } else {
        copy_h = 1;
        copy_w = std::min<uint32_t>(tex->width, static_cast<uint32_t>((avail / bsz) * bw));
    }
    if (copy_w == 0 || copy_h == 0) return;

    VkImageLayout old_l = tex->mip_layouts.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : tex->mip_layouts[0];
    transition_image_layout(cmd, tex->image, tex->aspect_mask, 0, 1, 0, 1,
                            old_l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy bic{};
    bic.bufferOffset = aligned_off;
    bic.bufferRowLength = (copy_h > 1) ? buf_row_len : 0;
    bic.bufferImageHeight = 0;
    bic.imageSubresource = {tex->aspect_mask, 0, 0, 1};
    bic.imageOffset = {0, 0, 0};
    bic.imageExtent = {copy_w, copy_h, 1};
    vkCmdCopyBufferToImage(cmd, buf->buffer, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

    transition_image_layout(cmd, tex->image, tex->aspect_mask, 0, 1, 0, 1,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!tex->mip_layouts.empty()) tex->mip_layouts[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static void flush_render_state(MtlRenderEncoderImpl *re, uint64_t primitive_type) {
    MtlVkState &st = get_state();
    if (!re || !re->inside_render_pass || !re->pipeline_state_obj) return;

    auto *pso = unwrap_mtl<MtlRenderPipelineStateImpl>(re->pipeline_state_obj);
    auto *dss = unwrap_mtl<MtlDepthStencilStateImpl>(re->depth_stencil_obj);
    if (!pso) return;

    if (pso->ps && (pso->ps->flags & SPIRV_ENTRY_FLAG_FB_FETCH) != 0) {
        const bool needs_rgb = (pso->ps->flags & SPIRV_ENTRY_FLAG_FB_FETCH_RGB) != 0;
        const bool need_snap = !re->fb_fetch_snap_valid || re->fb_fetch_alpha_dirty || (needs_rgb && re->fb_fetch_rgb_dirty);
        if (need_snap) {
            st.prof_fb_snaps.fetch_add(1, std::memory_order_relaxed);
            re->pass_fb_snaps++;
            snapshot_color0_for_fb_fetch(re);
            re->fb_fetch_snap_valid = true;
            re->fb_fetch_alpha_dirty = false;
            re->fb_fetch_rgb_dirty = false;
            re->fb_fetch_last_pso = re->pipeline_state_obj;
        }
    }
    /* Track which color[0] channels this draw actually modifies (accounting for alpha/rgb no-op blend states!)
       so subsequent depth-only (.a) FB_FETCH draws can reuse the snapshot without splitting the render pass. */
    if (re->color_count > 0) {
        const auto &pc0 = pso->desc.color[0];
        uint64_t wm0 = pc0.write_mask;
        if (wm0 == 0 && !pc0.blending_enabled && pc0.pixel_format == 0) wm0 = 0xFULL;
        const bool alpha_noop = pc0.blending_enabled &&
                                pc0.alpha_op == 0 /* Add */ &&
                                pc0.src_alpha == 0 /* Zero */ &&
                                pc0.dst_alpha == 1 /* One */;
        const bool rgb_noop   = pc0.blending_enabled &&
                                pc0.rgb_op == 0 /* Add */ &&
                                pc0.src_rgb == 0 /* Zero */ &&
                                pc0.dst_rgb == 1 /* One */;
        if ((wm0 & 0x1ULL) && !alpha_noop) re->fb_fetch_alpha_dirty = true; /* Apple MTLColorWriteMaskAlpha = 0x1 */
        if ((wm0 & 0xEULL) && !rgb_noop)   re->fb_fetch_rgb_dirty = true;   /* Apple MTLColorWriteMaskRGB   = 0xE */
    }
    re->pass_draws++;

    VkCommandBuffer cmd = re->cmd_buf->cmd;
    if (re->bound_pipeline == VK_NULL_HANDLE ||
        re->bound_pso_obj != re->pipeline_state_obj ||
        re->bound_dss_obj != re->depth_stencil_obj ||
        re->bound_ptype != primitive_type ||
        re->bound_cull != re->cull_mode ||
        re->bound_wind != re->front_winding ||
        re->bound_fill != re->fill_mode ||
        re->bound_depth_clip != re->depth_clip_mode) {
        VkPipeline vk_pipe = get_or_create_specialized_graphics_pipeline(
            pso, dss, primitive_type, re->cull_mode, re->front_winding, re->fill_mode, re->depth_clip_mode,
            re->color_formats, re->color_count, re->depth_format, re->stencil_format,
            (st.vk_ctx.caps().dynamic_rendering && st.vk_ctx.cmd_begin_rendering()) ? VK_NULL_HANDLE : re->fallback_render_pass);
        if (vk_pipe == VK_NULL_HANDLE) return;
        if (vk_pipe != re->bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipe);
            re->bound_pipeline = vk_pipe;
        }
        re->bound_pso_obj = re->pipeline_state_obj;
        re->bound_dss_obj = re->depth_stencil_obj;
        re->bound_ptype = primitive_type;
        re->bound_cull = re->cull_mode;
        re->bound_wind = re->front_winding;
        re->bound_fill = re->fill_mode;
        re->bound_depth_clip = re->depth_clip_mode;
    }

    if (re->has_viewport && re->viewport_dirty) {
        VkViewport vp{};
        vp.x = (float)re->viewport.originX;
        vp.y = (float)(re->viewport.originY + re->viewport.height);
        vp.width = (float)re->viewport.width;
        vp.height = -(float)re->viewport.height;
        vp.minDepth = (float)re->viewport.znear;
        vp.maxDepth = (float)re->viewport.zfar;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        re->viewport_dirty = false;
    }

    if (re->has_scissor && re->scissor_dirty) {
        int32_t sx = (int32_t)std::min<uint64_t>(re->scissor.x, re->render_w > 0 ? re->render_w - 1 : 0);
        int32_t sy = (int32_t)std::min<uint64_t>(re->scissor.y, re->render_h > 0 ? re->render_h - 1 : 0);
        uint32_t sw = (uint32_t)std::min<uint64_t>(re->scissor.width, re->render_w - (uint32_t)sx);
        uint32_t sh = (uint32_t)std::min<uint64_t>(re->scissor.height, re->render_h - (uint32_t)sy);
        VkRect2D sc{{sx, sy}, {std::max(1u, sw), std::max(1u, sh)}};
        vkCmdSetScissor(cmd, 0, 1, &sc);
        re->scissor_dirty = false;
    }

    if (re->depth_bias_dirty) {
        const float safe_clamp = (!st.vk_ctx.caps().supported_features.depthBiasClamp ||
                                  std::fabs(re->depth_clamp) > 1e6f)
                                     ? 0.0f
                                     : re->depth_clamp;
        vkCmdSetDepthBias(cmd, re->depth_bias, safe_clamp, re->depth_slope);
        re->depth_bias_dirty = false;
    }
    if (re->blend_constants_dirty) {
        vkCmdSetBlendConstants(cmd, re->blend_constants);
        re->blend_constants_dirty = false;
    }
    if (re->stencil_ref_dirty) {
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, re->stencil_ref);
        re->stencil_ref_dirty = false;
    }

    /* Bind only dirty vertex streams referenced by MTLVertexDescriptor */
    uint32_t vb_mask = pso->vertex_buffer_mask & re->vs.vb_dirty_mask;
    while (vb_mask != 0) {
        uint32_t b = (uint32_t)__builtin_ctz(vb_mask);
        vb_mask &= ~(1u << b);
        if (b < 31 && re->vs.buffer_bound[b] && re->vs.buffer[b] != VK_NULL_HANDLE) {
            VkBuffer vb = re->vs.buffer[b];
            VkDeviceSize voff = re->vs.buffer_offset[b];
            vkCmdBindVertexBuffers(cmd, b, 1, &vb, &voff);
            re->vs.vb_dirty_mask &= ~(1u << b);
        }
    }

    /* Overflow fallback if hardware occlusion query pool was exhausted */
    if (!re->occ_query_active && re->vis_mode != 0 && re->pass_desc.visibility_result_buffer_obj != nullptr) {
        uint64_t vis_val = (re->vis_mode == 2) ? 1000ULL : 1ULL;
        if (re->vis_writes.empty() || re->vis_writes.back().first != re->vis_offset) {
            re->vis_writes.push_back({re->vis_offset, vis_val});
        }
    }

    /* Allocate & bind Set 0 (VS) and Set 1 (PS) only when a slot used by this shader changed or layout changed */
    const bool vs_used_dirty = ((re->vs.dirty_buf_mask & pso->vs_used_buf_mask) != 0) ||
                               ((re->vs.dirty_tex_mask & pso->vs_used_tex_mask) != 0) ||
                               ((re->vs.dirty_smp_mask & pso->vs_used_smp_mask) != 0);
    const bool ps_used_dirty = ((re->ps.dirty_buf_mask & pso->ps_used_buf_mask) != 0) ||
                               ((re->ps.dirty_tex_mask & pso->ps_used_tex_mask) != 0) ||
                               ((re->ps.dirty_smp_mask & pso->ps_used_smp_mask) != 0);
    const bool need_set0 = vs_used_dirty || (pso->set0_layout != re->bound_set0_layout) || (re->bound_set0 == VK_NULL_HANDLE);
    const bool need_set1 = ps_used_dirty || (pso->set1_layout != re->bound_set1_layout) || (re->bound_set1 == VK_NULL_HANDLE);
    const bool layout_changed = (pso->pipeline_layout != re->bound_pipe_layout);

    VkDescriptorSet prev_set0 = re->bound_set0;
    VkDescriptorSet prev_set1 = re->bound_set1;
    uint32_t prev_dyn0[8], prev_dyn1[8];
    uint8_t prev_dyn0_cnt = re->bound_set0_dyn_count;
    uint8_t prev_dyn1_cnt = re->bound_set1_dyn_count;
    if (prev_dyn0_cnt > 0) std::memcpy(prev_dyn0, re->bound_set0_dyn_offsets, prev_dyn0_cnt * sizeof(uint32_t));
    if (prev_dyn1_cnt > 0) std::memcpy(prev_dyn1, re->bound_set1_dyn_offsets, prev_dyn1_cnt * sizeof(uint32_t));

    if (need_set0) {
        re->bound_set0 = allocate_and_write_set(re->cmd_buf, pso->set0_layout, pso->vs, re->vs,
                                                pso->vs_smp_tex_mask,
                                                pso->vs_dyn_ubo_mask, pso->vs_dyn_ubo_slots, pso->vs_dyn_ubo_count,
                                                re->bound_set0_dyn_offsets);
        re->bound_set0_layout = pso->set0_layout;
        re->bound_set0_dyn_count = pso->vs_dyn_ubo_count;
        re->vs.dirty_buf_mask &= ~pso->vs_used_buf_mask;
        re->vs.dirty_tex_mask &= ~pso->vs_used_tex_mask;
        re->vs.dirty_smp_mask &= ~pso->vs_used_smp_mask;
        re->vs.desc_dirty = false;
    }
    if (need_set1) {
        re->bound_set1 = allocate_and_write_set(re->cmd_buf, pso->set1_layout, pso->ps, re->ps,
                                                pso->ps_smp_tex_mask,
                                                pso->ps_dyn_ubo_mask, pso->ps_dyn_ubo_slots, pso->ps_dyn_ubo_count,
                                                re->bound_set1_dyn_offsets);
        re->bound_set1_layout = pso->set1_layout;
        re->bound_set1_dyn_count = pso->ps_dyn_ubo_count;
        re->ps.dirty_buf_mask &= ~pso->ps_used_buf_mask;
        re->ps.dirty_tex_mask &= ~pso->ps_used_tex_mask;
        re->ps.dirty_smp_mask &= ~pso->ps_used_smp_mask;
        re->ps.desc_dirty = false;
    }
    bool dyn_changed = (re->bound_set0_dyn_count != prev_dyn0_cnt) ||
                       (re->bound_set1_dyn_count != prev_dyn1_cnt) ||
                       (re->bound_set0_dyn_count > 0 && std::memcmp(re->bound_set0_dyn_offsets, prev_dyn0, re->bound_set0_dyn_count * sizeof(uint32_t)) != 0) ||
                       (re->bound_set1_dyn_count > 0 && std::memcmp(re->bound_set1_dyn_offsets, prev_dyn1, re->bound_set1_dyn_count * sizeof(uint32_t)) != 0);

    if ((re->bound_set0 != prev_set0 || re->bound_set1 != prev_set1 || layout_changed || dyn_changed) &&
        re->bound_set0 != VK_NULL_HANDLE && re->bound_set1 != VK_NULL_HANDLE) {
        VkDescriptorSet sets[2] = {re->bound_set0, re->bound_set1};
        uint32_t all_dyn[16];
        uint32_t total_dyn = 0;
        for (uint8_t d = 0; d < re->bound_set0_dyn_count && total_dyn < 16; ++d) {
            all_dyn[total_dyn++] = re->bound_set0_dyn_offsets[d];
        }
        for (uint8_t d = 0; d < re->bound_set1_dyn_count && total_dyn < 16; ++d) {
            all_dyn[total_dyn++] = re->bound_set1_dyn_offsets[d];
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso->pipeline_layout, 0, 2, sets,
                                total_dyn, total_dyn > 0 ? all_dyn : nullptr);
        re->bound_pipe_layout = pso->pipeline_layout;
    }
}

static void mtl_re_dealloc(void *self) {
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(self);
    if (!re) return;
    for (VkImageView v : re->attachment_views_to_destroy) {
        enqueue_deferred_view(v);
    }
    re->attachment_views_to_destroy.clear();
    if (re->fallback_framebuffer != VK_NULL_HANDLE) {
        enqueue_deferred_framebuffer(re->fallback_framebuffer);
        re->fallback_framebuffer = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        if (re->pass_desc.color[i].texture_obj) {
            oh2_objc_release_object(re->pass_desc.color[i].texture_obj);
        }
        if (re->pass_desc.color[i].resolve_texture_obj) {
            oh2_objc_release_object(re->pass_desc.color[i].resolve_texture_obj);
        }
    }
    if (re->pass_desc.depth.texture_obj) {
        oh2_objc_release_object(re->pass_desc.depth.texture_obj);
    }
    if (re->pass_desc.depth.resolve_texture_obj) {
        oh2_objc_release_object(re->pass_desc.depth.resolve_texture_obj);
    }
    if (re->pass_desc.stencil.texture_obj) {
        oh2_objc_release_object(re->pass_desc.stencil.texture_obj);
    }
    if (re->pass_desc.visibility_result_buffer_obj) {
        oh2_objc_release_object(re->pass_desc.visibility_result_buffer_obj);
    }
    get_state().live_encoders.fetch_sub(1, std::memory_order_relaxed);
    delete re;
}

static void *mtl_cb_renderEncoder(void *self, const char *sel, void *rpd_obj) {
    (void)sel;
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(self);
    auto *rpd = unwrap_mtl<MtlRenderPassDescImpl>(rpd_obj);
    if (!cb || !rpd) return nullptr;

    auto *re = new MtlRenderEncoderImpl();
    re->cmd_buf_obj = self;
    re->cmd_buf = cb;
    re->pass_desc = *rpd;
    re->pass_desc.color_array_obj = nullptr;
    re->pass_desc.depth_wrap = nullptr;
    re->pass_desc.stencil_wrap = nullptr;
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        re->pass_desc.color_wraps[i] = nullptr;
        if (re->pass_desc.color[i].texture_obj) {
            oh2_objc_add_refcount(re->pass_desc.color[i].texture_obj, 1);
        }
        if (re->pass_desc.color[i].resolve_texture_obj) {
            oh2_objc_add_refcount(re->pass_desc.color[i].resolve_texture_obj, 1);
        }
    }
    if (re->pass_desc.depth.texture_obj) {
        oh2_objc_add_refcount(re->pass_desc.depth.texture_obj, 1);
    }
    if (re->pass_desc.depth.resolve_texture_obj) {
        oh2_objc_add_refcount(re->pass_desc.depth.resolve_texture_obj, 1);
    }
    if (re->pass_desc.stencil.texture_obj) {
        oh2_objc_add_refcount(re->pass_desc.stencil.texture_obj, 1);
    }
    if (re->pass_desc.visibility_result_buffer_obj) {
        oh2_objc_add_refcount(re->pass_desc.visibility_result_buffer_obj, 1);
    }
    if (check_begin_cb(cb)) {
        mtl_flush_pending_tex_uploads_cmd(cb);
        FrameSyncSlot &slot = get_state().frames[cb->frame_idx];
        if (slot.ts_capturing && slot.ts_query_pool != VK_NULL_HANDLE &&
            slot.ts_query_cursor + 2 <= kMaxTimestampQueriesPerCB) {
            TsPassRecord rec{};
            rec.start_q = slot.ts_query_cursor++;
            rec.end_q = rec.start_q;
            vkCmdWriteTimestamp(cb->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.ts_query_pool, rec.start_q);
            re->ts_pass_idx = (uint32_t)slot.ts_passes.size();
            slot.ts_passes.push_back(rec);
        }
    }
    get_state().prof_render_passes.fetch_add(1, std::memory_order_relaxed);
    begin_render_pass_internal(re);
    if (re->ts_pass_idx != UINT32_MAX) {
        FrameSyncSlot &slot = get_state().frames[cb->frame_idx];
        if (re->ts_pass_idx < slot.ts_passes.size()) {
            slot.ts_passes[re->ts_pass_idx].w = re->render_w;
            slot.ts_passes[re->ts_pass_idx].h = re->render_h;
            slot.ts_passes[re->ts_pass_idx].c_cnt = re->color_count;
            slot.ts_passes[re->ts_pass_idx].dfmt = (int)re->depth_format;
        }
    }
    get_state().live_encoders.fetch_add(1, std::memory_order_relaxed);
    return alloc_mtl_wrap_managed("MTLRenderCommandEncoder", re, &mtl_re_dealloc, true);
}

static void mtl_re_setPSO(void *s, const char *sel, void *pso) { (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) re->pipeline_state_obj = pso; }
static void mtl_re_setDSS(void *s, const char *sel, void *dss) { (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) re->depth_stencil_obj = dss; }
static void mtl_re_setCull(void *s, const char *sel, uint64_t m) { (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) re->cull_mode = m; }
static void mtl_re_setWind(void *s, const char *sel, uint64_t w) { (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) re->front_winding = w; }
static void mtl_re_setFill(void *s, const char *sel, uint64_t m) { (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) re->fill_mode = m; }
static void mtl_re_setStencilRef(void *s, const char *sel, uint32_t v) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) {
        if (re->stencil_ref != v) { re->stencil_ref = v; re->stencil_ref_dirty = true; }
    }
}
static void mtl_re_setStencilFB(void *s, const char *sel, uint32_t f, uint32_t b) {
    (void)sel; (void)b;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) {
        if (re->stencil_ref != f) { re->stencil_ref = f; re->stencil_ref_dirty = true; }
    }
}
static void mtl_re_setDepthBias(void *s, const char *sel, float b, float sl, float cl) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) {
        if (re->depth_bias != b || re->depth_slope != sl || re->depth_clamp != cl) {
            re->depth_bias = b;
            re->depth_slope = sl;
            re->depth_clamp = cl;
            re->depth_bias_dirty = true;
        }
    }
}

static void mtl_re_setViewport(void *s, const char *sel, MTLViewport *vp) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s); re && vp) {
        re->viewport = *vp;
        re->has_viewport = true;
        re->viewport_dirty = true;
    }
}
static void mtl_re_setViewports(void *s, const char *sel, MTLViewport *vps, uint64_t cnt) {
    (void)sel;
    if (cnt > 0 && vps) mtl_re_setViewport(s, sel, &vps[0]);
}
static void mtl_re_setScissor(void *s, const char *sel, MTLScissorRect *sr) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s); re && sr) {
        re->scissor = *sr;
        re->has_scissor = true;
        re->scissor_dirty = true;
    }
}
static void mtl_re_setDepthClipMode(void *s, const char *sel, uint64_t mode) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) {
        re->depth_clip_mode = mode;
    }
}
static void mtl_re_setScissors(void *s, const char *sel, MTLScissorRect *srs, uint64_t cnt) {
    (void)sel;
    if (cnt > 0 && srs) mtl_re_setScissor(s, sel, &srs[0]);
}

static void bind_stage_buffer(MtlStageBindings &sb, uint64_t idx, void *buf_obj, uint64_t off) {
    if (idx >= kMaxBuffers) return;
    auto *b = unwrap_mtl<MtlBufferImpl>(buf_obj);
    if (b && b->buffer != VK_NULL_HANDLE) {
        if (sb.buffer_bound[idx] && sb.buffer[idx] == b->buffer && sb.buffer_offset[idx] == off && sb.buffer_total_len[idx] == b->length) {
            return;
        }
        sb.buffer[idx] = b->buffer;
        sb.buffer_uid[idx] = b->unique_id;
        sb.buffer_offset[idx] = off;
        sb.buffer_total_len[idx] = b->length;
        sb.buffer_range[idx] = (b->length > off) ? (b->length - off) : VK_WHOLE_SIZE;
        sb.buffer_mapped[idx] = static_cast<uint8_t *>(b->mapped);
        sb.buffer_obj[idx] = buf_obj;
        sb.buffer_bound[idx] = true;
    } else {
        if (!sb.buffer_bound[idx]) return;
        sb.buffer_uid[idx] = 0;
        sb.buffer_total_len[idx] = 0;
        sb.buffer_mapped[idx] = nullptr;
        sb.buffer_obj[idx] = nullptr;
        sb.buffer_bound[idx] = false;
    }
    sb.desc_dirty = true;
    sb.dirty_buf_mask |= (1u << idx);
    sb.vb_dirty_mask |= (1u << idx);
}

static void set_stage_buffer_offset(MtlStageBindings &sb, uint64_t idx, uint64_t off) {
    if (idx >= kMaxBuffers) return;
    if (sb.buffer_offset[idx] == off) return;
    sb.buffer_offset[idx] = off;
    if (sb.buffer_total_len[idx] > 0) {
        sb.buffer_range[idx] = (sb.buffer_total_len[idx] > off) ? (sb.buffer_total_len[idx] - off) : VK_WHOLE_SIZE;
    }
    sb.desc_dirty = true;
    sb.dirty_buf_mask |= (1u << idx);
    sb.vb_dirty_mask |= (1u << idx);
}

static void bind_stage_bytes(MtlCommandBufferImpl *cb, MtlStageBindings &sb, const void *ptr, uint64_t len, uint64_t idx) {
    if (idx >= kMaxBuffers || !cb || !ptr || len == 0) return;
    VkBuffer vbuf = VK_NULL_HANDLE;
    uint64_t voff = 0;
    if (mtl_alloc_inline_bytes(cb, ptr, len, &vbuf, &voff)) {
        sb.buffer[idx] = vbuf;
        sb.buffer_uid[idx] = (uint64_t)(uintptr_t)vbuf;
        sb.buffer_offset[idx] = voff;
        sb.buffer_total_len[idx] = voff + len;
        sb.buffer_range[idx] = len;
        sb.buffer_mapped[idx] = get_state().frames[cb->frame_idx].inline_mapped;
        sb.buffer_obj[idx] = nullptr;
        sb.buffer_bound[idx] = true;
        sb.desc_dirty = true;
        sb.dirty_buf_mask |= (1u << idx);
        sb.vb_dirty_mask |= (1u << idx);
    }
}

static void bind_stage_texture(MtlCommandBufferImpl *cb, MtlStageBindings &sb, uint64_t idx, void *tex_obj, bool queue_btex = true) {
    if (idx >= kMaxTextures) return;
    auto *t = unwrap_mtl<MtlTextureImpl>(tex_obj);
    if (t && t->view != VK_NULL_HANDLE) {
        if (queue_btex && t->parent_buffer_obj && cb) {
            mtl_queue_btex_for_preamble(cb, t);
        }
        if (sb.tex_bound[idx] && sb.tex_view[idx] == t->view && sb.tex_uid[idx] == t->unique_id) return;
        sb.tex_view[idx] = t->view;
        sb.tex_uid[idx] = t->unique_id;
        sb.tex_obj[idx] = tex_obj;
        const bool is_d = (t->aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        sb.tex_is_depth[idx] = is_d;
        if (is_d) sb.depth_tex_mask |= (1u << idx);
        else      sb.depth_tex_mask &= ~(1u << idx);
        sb.tex_bound[idx] = true;
    } else {
        if (!sb.tex_bound[idx]) return;
        sb.tex_uid[idx] = 0;
        sb.tex_obj[idx] = nullptr;
        sb.tex_is_depth[idx] = false;
        sb.depth_tex_mask &= ~(1u << idx);
        sb.tex_bound[idx] = false;
    }
    sb.desc_dirty = true;
    sb.dirty_tex_mask |= (1u << idx);
}

static void bind_stage_sampler(MtlStageBindings &sb, uint64_t idx, void *smp_obj) {
    if (idx >= kMaxSamplers) return;
    auto *smp = unwrap_mtl<MtlSamplerImpl>(smp_obj);
    if (smp && smp->sampler != VK_NULL_HANDLE) {
        if (sb.sampler_bound[idx] && sb.sampler[idx] == smp->sampler) return;
        sb.sampler[idx] = smp->sampler;
        sb.sampler_nearest[idx] = smp->nearest_sampler;
        sb.sampler_obj[idx] = smp_obj;
        sb.sampler_bound[idx] = true;
    } else {
        if (!sb.sampler_bound[idx]) return;
        sb.sampler_nearest[idx] = VK_NULL_HANDLE;
        sb.sampler_obj[idx] = nullptr;
        sb.sampler_bound[idx] = false;
    }
    sb.desc_dirty = true;
    sb.dirty_smp_mask |= (1u << idx);
}

static void mtl_re_setVB(void *s, const char *sel, void *b, uint64_t off, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) bind_stage_buffer(re->vs, idx, b, off);
}
static void mtl_re_setVBO(void *s, const char *sel, uint64_t off, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) set_stage_buffer_offset(re->vs, idx, off);
}
static void mtl_re_setVBytes(void *s, const char *sel, const void *ptr, uint64_t len, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) bind_stage_bytes(re->cmd_buf, re->vs, ptr, len, idx);
}
static void mtl_re_setVT(void *s, const char *sel, void *t, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) bind_stage_texture(re->cmd_buf, re->vs, idx, t, true);
}
static void mtl_re_setVS(void *s, const char *sel, void *smp, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) bind_stage_sampler(re->vs, idx, smp);
}

static void mtl_re_setFB(void *s, const char *sel, void *b, uint64_t off, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) bind_stage_buffer(re->ps, idx, b, off);
}
static void mtl_re_setFBO(void *s, const char *sel, uint64_t off, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) set_stage_buffer_offset(re->ps, idx, off);
}
static void mtl_re_setFBytes(void *s, const char *sel, const void *ptr, uint64_t len, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) bind_stage_bytes(re->cmd_buf, re->ps, ptr, len, idx);
}
static void mtl_re_setFT(void *s, const char *sel, void *t, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) bind_stage_texture(re->cmd_buf, re->ps, idx, t, true);
}
static void mtl_re_setFS(void *s, const char *sel, void *smp, uint64_t idx) {
    (void)sel; if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) bind_stage_sampler(re->ps, idx, smp);
}

static void mtl_re_drawInstBase(void *s, const char *sel, uint64_t ptype, uint64_t vstart,
                                uint64_t vcount, uint64_t icount, uint64_t baseInst) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re || !re->inside_render_pass || vcount == 0 || icount == 0) return;
    flush_render_state(re, ptype);
    vkCmdDraw(re->cmd_buf->cmd, (uint32_t)vcount, (uint32_t)icount, (uint32_t)vstart, (uint32_t)baseInst);
    get_state().draw_calls.fetch_add(1, std::memory_order_relaxed);
}
static void mtl_re_drawInst(void *s, const char *sel, uint64_t ptype, uint64_t vstart, uint64_t vcount, uint64_t icount) {
    mtl_re_drawInstBase(s, sel, ptype, vstart, vcount, icount, 0);
}
static void mtl_re_draw(void *s, const char *sel, uint64_t ptype, uint64_t vstart, uint64_t vcount) {
    mtl_re_drawInstBase(s, sel, ptype, vstart, vcount, 1, 0);
}

static void mtl_re_drawIdxFull(void *s, const char *sel, uint64_t ptype, uint64_t icount, uint64_t itype,
                               void *ibuf_obj, uint64_t ibuf_off, uint64_t inst_cnt, int64_t base_vtx, uint64_t base_inst) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    auto *ib = unwrap_mtl<MtlBufferImpl>(ibuf_obj);
    if (!re || !re->inside_render_pass || !ib || ib->buffer == VK_NULL_HANDLE || icount == 0 || inst_cnt == 0) return;
    flush_render_state(re, ptype);
    VkIndexType vk_itype = (itype == kMTLIndexUInt32) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    vkCmdBindIndexBuffer(re->cmd_buf->cmd, ib->buffer, ibuf_off, vk_itype);
    vkCmdDrawIndexed(re->cmd_buf->cmd, (uint32_t)icount, (uint32_t)inst_cnt, 0, (int32_t)base_vtx, (uint32_t)base_inst);
    get_state().indexed_draw_calls.fetch_add(1, std::memory_order_relaxed);
}
static void mtl_re_drawIdxInst(void *s, const char *sel, uint64_t ptype, uint64_t icount, uint64_t itype,
                               void *ibuf_obj, uint64_t ibuf_off, uint64_t inst_cnt) {
    mtl_re_drawIdxFull(s, sel, ptype, icount, itype, ibuf_obj, ibuf_off, inst_cnt, 0, 0);
}
static void mtl_re_drawIdx(void *s, const char *sel, uint64_t ptype, uint64_t icount, uint64_t itype,
                           void *ibuf_obj, uint64_t ibuf_off) {
    mtl_re_drawIdxFull(s, sel, ptype, icount, itype, ibuf_obj, ibuf_off, 1, 0, 0);
}

static void mtl_re_endEncoding(void *s, const char *sel) {
    (void)sel;
    MtlVkState &st = get_state();
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re || !re->inside_render_pass) return;
    if (re->occ_query_active && re->cmd_buf) {
        FrameSyncSlot &slot = st.frames[re->cmd_buf->frame_idx];
        if (slot.occ_query_pool != VK_NULL_HANDLE) {
            vkCmdEndQuery(re->cmd_buf->cmd, slot.occ_query_pool, re->occ_active_qidx);
        }
        re->occ_query_active = false;
    }
    if (st.vk_ctx.caps().dynamic_rendering && st.vk_ctx.cmd_end_rendering()) {
        st.vk_ctx.cmd_end_rendering()(re->cmd_buf->cmd);
    } else {
        vkCmdEndRenderPass(re->cmd_buf->cmd);
    }
    re->inside_render_pass = false;

    if (re->ts_pass_idx != UINT32_MAX && re->cmd_buf) {
        FrameSyncSlot &slot = st.frames[re->cmd_buf->frame_idx];
        if (slot.ts_capturing && slot.ts_query_pool != VK_NULL_HANDLE &&
            re->ts_pass_idx < slot.ts_passes.size() &&
            slot.ts_query_cursor < kMaxTimestampQueriesPerCB) {
            uint32_t eq = slot.ts_query_cursor++;
            vkCmdWriteTimestamp(re->cmd_buf->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.ts_query_pool, eq);
            auto &rec = slot.ts_passes[re->ts_pass_idx];
            rec.end_q = eq;
            rec.draws = re->pass_draws;
            rec.fb_snaps = re->pass_fb_snaps;
            rec.occs = (uint32_t)re->occ_queries.size();
        }
        re->ts_pass_idx = UINT32_MAX;
    }

    /* Copy hardware occlusion query results into visibilityResultBuffer outside render pass */
    if ((!re->occ_queries.empty() || !re->vis_writes.empty()) && re->pass_desc.visibility_result_buffer_obj) {
        auto *vbuf = unwrap_mtl<MtlBufferImpl>(re->pass_desc.visibility_result_buffer_obj);
        if (vbuf && vbuf->buffer != VK_NULL_HANDLE) {
            FrameSyncSlot &slot = st.frames[re->cmd_buf->frame_idx];
            bool any_updated = false;
            if (slot.occ_query_pool != VK_NULL_HANDLE) {
                const size_t nq = re->occ_queries.size();
                size_t qi = 0;
                while (qi < nq) {
                    uint32_t start_q = re->occ_queries[qi].first;
                    uint64_t start_off = re->occ_queries[qi].second;
                    if ((start_off & 7ULL) != 0 || start_off + sizeof(uint64_t) > vbuf->length) {
                        qi++;
                        continue;
                    }
                    uint32_t run = 1;
                    while (qi + run < nq) {
                        uint32_t nq_id = re->occ_queries[qi + run].first;
                        uint64_t nq_off = re->occ_queries[qi + run].second;
                        if (nq_id == start_q + run && nq_off == start_off + (uint64_t)run * sizeof(uint64_t) &&
                            nq_off + sizeof(uint64_t) <= vbuf->length) {
                            run++;
                        } else {
                            break;
                        }
                    }
                    vkCmdCopyQueryPoolResults(
                        re->cmd_buf->cmd, slot.occ_query_pool, start_q, run,
                        vbuf->buffer, start_off, sizeof(uint64_t),
                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                    any_updated = true;
                    qi += run;
                }
            }
            for (const auto &vw : re->vis_writes) {
                uint64_t off = vw.first;
                uint64_t val = vw.second;
                if ((off & 3ULL) == 0 && off + sizeof(uint64_t) <= vbuf->length) {
                    vkCmdUpdateBuffer(re->cmd_buf->cmd, vbuf->buffer, off, sizeof(uint64_t), &val);
                    any_updated = true;
                }
            }
            if (any_updated) {
                vbuf->needs_host_invalidate.store(true, std::memory_order_release);
                VkMemoryBarrier vmb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                vmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vmb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(re->cmd_buf->cmd,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 1, &vmb, 0, nullptr, 0, nullptr);
            }
        }
        re->occ_queries.clear();
        re->vis_writes.clear();
    }

    for (VkImageView v : re->attachment_views_to_destroy) {
        enqueue_deferred_view(v);
    }
    re->attachment_views_to_destroy.clear();
    if (re->fallback_framebuffer != VK_NULL_HANDLE) {
        enqueue_deferred_framebuffer(re->fallback_framebuffer);
        re->fallback_framebuffer = VK_NULL_HANDLE;
    }

    /* Resolve attachments (if resolveTexture specified) and transition offscreen targets to SHADER_READ_ONLY_OPTIMAL
       so subsequent passes can sample or reload cleanly without image-layout validation mismatches. */
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        const auto &ca = re->pass_desc.color[i];
        if (!ca.texture_obj) continue;
        auto *tex = unwrap_mtl<MtlTextureImpl>(ca.texture_obj);
        if (!tex || tex->image == VK_NULL_HANDLE) continue;
        uint32_t lvl = (uint32_t)ca.level;
        uint32_t slc = (uint32_t)ca.slice;

        if (ca.resolve_texture_obj && ca.resolve_texture_obj != ca.texture_obj &&
            ca.store_action != kMTLStoreActionDontCare && ca.store_action != kMTLStoreActionStore) {
            auto *rtex = unwrap_mtl<MtlTextureImpl>(ca.resolve_texture_obj);
            if (rtex && rtex->image != VK_NULL_HANDLE) {
                uint32_t rlvl = (uint32_t)ca.resolve_level;
                uint32_t rslc = (uint32_t)ca.resolve_slice;
                uint32_t sw = std::max(1u, tex->width >> lvl);
                uint32_t sh = std::max(1u, tex->height >> lvl);
                uint32_t dw = std::max(1u, rtex->width >> rlvl);
                uint32_t dh = std::max(1u, rtex->height >> rlvl);

                transition_image_layout(re->cmd_buf->cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        lvl, 1, slc, 1,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                transition_image_layout(re->cmd_buf->cmd, rtex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        rlvl, 1, rslc, 1,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                if (tex->vk_format == rtex->vk_format && sw == dw && sh == dh) {
                    VkImageCopy ic{};
                    ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, slc, 1};
                    ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, rlvl, rslc, 1};
                    ic.extent = {sw, sh, 1};
                    vkCmdCopyImage(re->cmd_buf->cmd,
                                   tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   rtex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &ic);
                } else {
                    VkImageBlit ib{};
                    ib.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, slc, 1};
                    ib.srcOffsets[1] = {(int32_t)sw, (int32_t)sh, 1};
                    ib.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, rlvl, rslc, 1};
                    ib.dstOffsets[1] = {(int32_t)dw, (int32_t)dh, 1};
                    vkCmdBlitImage(re->cmd_buf->cmd,
                                   tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   rtex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &ib, VK_FILTER_LINEAR);
                }

                VkImageLayout tex_after = tex->is_swapchain ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                VkImageLayout rtex_after = rtex->is_swapchain ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                transition_image_layout(re->cmd_buf->cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        lvl, 1, slc, 1,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_after);
                transition_image_layout(re->cmd_buf->cmd, rtex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        rlvl, 1, rslc, 1,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rtex_after);
                tex->set_subres_layout(lvl, slc, tex_after);
                rtex->set_subres_layout(rlvl, rslc, rtex_after);
                continue;
            }
        }

        if (tex->is_swapchain) continue;
        transition_image_layout(re->cmd_buf->cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                lvl, 1, slc, 1,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tex->set_subres_layout(lvl, slc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    if (re->pass_desc.depth.texture_obj) {
        auto *dtex = unwrap_mtl<MtlTextureImpl>(re->pass_desc.depth.texture_obj);
        if (dtex && dtex->image != VK_NULL_HANDLE) {
            uint32_t dlvl = (uint32_t)re->pass_desc.depth.level;
            uint32_t dslc = (uint32_t)re->pass_desc.depth.slice;
            if (re->pass_desc.depth.resolve_texture_obj && re->pass_desc.depth.resolve_texture_obj != re->pass_desc.depth.texture_obj) {
                auto *rdtex = unwrap_mtl<MtlTextureImpl>(re->pass_desc.depth.resolve_texture_obj);
                if (rdtex && rdtex->image != VK_NULL_HANDLE && dtex->vk_format == rdtex->vk_format && dtex->aspect_mask == rdtex->aspect_mask) {
                    uint32_t rlvl = (uint32_t)re->pass_desc.depth.resolve_level;
                    uint32_t rslc = (uint32_t)re->pass_desc.depth.resolve_slice;
                    uint32_t sw = std::max(1u, dtex->width >> dlvl);
                    uint32_t sh = std::max(1u, dtex->height >> dlvl);
                    uint32_t dw = std::max(1u, rdtex->width >> rlvl);
                    uint32_t dh = std::max(1u, rdtex->height >> rlvl);
                    if (sw == dw && sh == dh) {
                        transition_image_layout(re->cmd_buf->cmd, dtex->image, dtex->aspect_mask,
                                                dlvl, 1, dslc, 1,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        transition_image_layout(re->cmd_buf->cmd, rdtex->image, rdtex->aspect_mask,
                                                rlvl, 1, rslc, 1,
                                                VK_IMAGE_LAYOUT_UNDEFINED,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy dic{};
                        dic.srcSubresource = {dtex->aspect_mask, dlvl, dslc, 1};
                        dic.dstSubresource = {rdtex->aspect_mask, rlvl, rslc, 1};
                        dic.extent = {sw, sh, 1};
                        vkCmdCopyImage(re->cmd_buf->cmd,
                                       dtex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       rdtex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &dic);
                        transition_image_layout(re->cmd_buf->cmd, dtex->image, dtex->aspect_mask,
                                                dlvl, 1, dslc, 1,
                                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        transition_image_layout(re->cmd_buf->cmd, rdtex->image, rdtex->aspect_mask,
                                                rlvl, 1, rslc, 1,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        dtex->set_subres_layout(dlvl, dslc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        rdtex->set_subres_layout(rlvl, rslc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        dtex = nullptr;
                    }
                }
            }
            if (dtex) {
                transition_image_layout(re->cmd_buf->cmd, dtex->image, dtex->aspect_mask,
                                        dlvl, 1, dslc, 1,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                dtex->set_subres_layout(dlvl, dslc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }
}

static void mtl_enc_noop(void *s, const char *sel) { (void)s; (void)sel; }
static void mtl_enc_setLabel(void *s, const char *sel, void *l) { (void)s; (void)sel; (void)l; }
static void *mtl_enc_device(void *s, const char *sel) { (void)s; (void)sel; return get_state().device_obj; }
static void *mtl_enc_label(void *s, const char *sel) { (void)s; (void)sel; return oh2_objc_create_string("MTLEncoder"); }
static uint64_t mtl_enc_zero(void *s, const char *sel) { (void)s; (void)sel; return 0ULL; }
static void mtl_enc_noop_u64(void *s, const char *sel, uint64_t a) { (void)s; (void)sel; (void)a; }
static void mtl_enc_noop_u64_u64(void *s, const char *sel, uint64_t a, uint64_t b) { (void)s; (void)sel; (void)a; (void)b; }
static void mtl_enc_noop_ptr_u64(void *s, const char *sel, void *a, uint64_t b) { (void)s; (void)sel; (void)a; (void)b; }
static void mtl_enc_noop_ptr_cnt_u64(void *s, const char *sel, void *a, uint64_t b, uint64_t c) { (void)s; (void)sel; (void)a; (void)b; (void)c; }
static void mtl_enc_noop_ptr_slc(void *s, const char *sel, void *a, uint64_t b, uint64_t c) { (void)s; (void)sel; (void)a; (void)b; (void)c; }
static void mtl_re_setColorStoreAction(void *s, const char *sel, uint64_t action, uint64_t idx) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s); re && idx < kMaxColorAttachments) {
        re->pass_desc.color[idx].store_action = action;
    }
}
static void mtl_re_setDepthStoreAction(void *s, const char *sel, uint64_t action) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) re->pass_desc.depth.store_action = action;
}
static void mtl_re_setStencilStoreAction(void *s, const char *sel, uint64_t action) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) re->pass_desc.stencil.store_action = action;
}
static void mtl_re_setVisResultMode(void *s, const char *sel, uint64_t mode, uint64_t offset) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re) return;
    MtlVkState &st = get_state();
    if (re->occ_query_active && re->cmd_buf) {
        FrameSyncSlot &slot = st.frames[re->cmd_buf->frame_idx];
        if (slot.occ_query_pool != VK_NULL_HANDLE) {
            vkCmdEndQuery(re->cmd_buf->cmd, slot.occ_query_pool, re->occ_active_qidx);
        }
        re->occ_query_active = false;
    }
    re->vis_mode = mode;
    re->vis_offset = offset;
    if (mode != 0 && re->inside_render_pass && re->cmd_buf && re->pass_desc.visibility_result_buffer_obj) {
        FrameSyncSlot &slot = st.frames[re->cmd_buf->frame_idx];
        if (slot.occ_query_pool != VK_NULL_HANDLE && slot.occ_query_cursor < kMaxOcclusionQueriesPerCB) {
            uint32_t qidx = slot.occ_query_cursor++;
            VkQueryControlFlags qflags = (mode == 2 && st.vk_ctx.caps().supported_features.occlusionQueryPrecise)
                                             ? VK_QUERY_CONTROL_PRECISE_BIT
                                             : 0;
            vkCmdBeginQuery(re->cmd_buf->cmd, slot.occ_query_pool, qidx, qflags);
            re->occ_query_active = true;
            re->occ_active_qidx = qidx;
            re->occ_queries.push_back({qidx, offset});
            st.prof_occ_queries.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
static void mtl_re_setBlendColor(void *s, const char *sel, float r, float g, float b, float a) {
    (void)sel;
    if (auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s)) {
        if (re->blend_constants[0] != r || re->blend_constants[1] != g ||
            re->blend_constants[2] != b || re->blend_constants[3] != a) {
            re->blend_constants[0] = r;
            re->blend_constants[1] = g;
            re->blend_constants[2] = b;
            re->blend_constants[3] = a;
            re->blend_constants_dirty = true;
        }
    }
}

static void mtl_re_setVBs(void *s, const char *sel, void *const *bufs, const uint64_t *offs, NSRange64 r) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re || !bufs) return;
    for (uint64_t i = 0; i < r.length; ++i) {
        bind_stage_buffer(re->vs, r.location + i, bufs[i], offs ? offs[i] : 0ULL);
    }
}
static void mtl_re_setVTs(void *s, const char *sel, void *const *texs, NSRange64 r) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re || !texs) return;
    for (uint64_t i = 0; i < r.length; ++i) {
        bind_stage_texture(re->cmd_buf, re->vs, r.location + i, texs[i], true);
    }
}
static void mtl_re_setVSs(void *s, const char *sel, void *const *smps, NSRange64 r) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re || !smps) return;
    for (uint64_t i = 0; i < r.length; ++i) {
        bind_stage_sampler(re->vs, r.location + i, smps[i]);
    }
}
static void mtl_re_setVSClamps(void *s, const char *sel, void *smp, float lmin, float lmax, uint64_t idx) {
    (void)lmin; (void)lmax;
    mtl_re_setVS(s, sel, smp, idx);
}
static void mtl_re_setVSsClamps(void *s, const char *sel, void *const *smps, const float *lmin, const float *lmax, NSRange64 r) {
    (void)lmin; (void)lmax;
    mtl_re_setVSs(s, sel, smps, r);
}

static void mtl_re_setFBs(void *s, const char *sel, void *const *bufs, const uint64_t *offs, NSRange64 r) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re || !bufs) return;
    for (uint64_t i = 0; i < r.length; ++i) {
        bind_stage_buffer(re->ps, r.location + i, bufs[i], offs ? offs[i] : 0ULL);
    }
}
static void mtl_re_setFTs(void *s, const char *sel, void *const *texs, NSRange64 r) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re || !texs) return;
    for (uint64_t i = 0; i < r.length; ++i) {
        bind_stage_texture(re->cmd_buf, re->ps, r.location + i, texs[i], true);
    }
}
static void mtl_re_setFSs(void *s, const char *sel, void *const *smps, NSRange64 r) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    if (!re || !smps) return;
    for (uint64_t i = 0; i < r.length; ++i) {
        bind_stage_sampler(re->ps, r.location + i, smps[i]);
    }
}
static void mtl_re_setFSClamps(void *s, const char *sel, void *smp, float lmin, float lmax, uint64_t idx) {
    (void)lmin; (void)lmax;
    mtl_re_setFS(s, sel, smp, idx);
}
static void mtl_re_setFSsClamps(void *s, const char *sel, void *const *smps, const float *lmin, const float *lmax, NSRange64 r) {
    (void)lmin; (void)lmax;
    mtl_re_setFSs(s, sel, smps, r);
}

static void mtl_re_drawIndirect(void *s, const char *sel, uint64_t ptype, void *ind_obj, uint64_t ind_off) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    auto *ind = unwrap_mtl<MtlBufferImpl>(ind_obj);
    if (!re || !re->inside_render_pass || !ind || ind->buffer == VK_NULL_HANDLE) return;
    flush_render_state(re, ptype);
    vkCmdDrawIndirect(re->cmd_buf->cmd, ind->buffer, ind_off, 1, 0);
    get_state().draw_calls.fetch_add(1, std::memory_order_relaxed);
}
static void mtl_re_drawIdxIndirect(void *s, const char *sel, uint64_t ptype, uint64_t itype,
                                   void *ibuf_obj, uint64_t ibuf_off, void *ind_obj, uint64_t ind_off) {
    (void)sel;
    auto *re = unwrap_mtl<MtlRenderEncoderImpl>(s);
    auto *ib = unwrap_mtl<MtlBufferImpl>(ibuf_obj);
    auto *ind = unwrap_mtl<MtlBufferImpl>(ind_obj);
    if (!re || !re->inside_render_pass || !ib || ib->buffer == VK_NULL_HANDLE || !ind || ind->buffer == VK_NULL_HANDLE) return;
    flush_render_state(re, ptype);
    VkIndexType vk_itype = (itype == kMTLIndexUInt32) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    vkCmdBindIndexBuffer(re->cmd_buf->cmd, ib->buffer, ibuf_off, vk_itype);
    vkCmdDrawIndexedIndirect(re->cmd_buf->cmd, ind->buffer, ind_off, 1, 0);
    get_state().indexed_draw_calls.fetch_add(1, std::memory_order_relaxed);
}
static void mtl_re_noop_tess(void *s, const char *sel, uint64_t a, uint64_t b, uint64_t c, void *d, uint64_t e, uint64_t f, uint64_t g) {
    (void)s; (void)sel; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
}
static void mtl_re_noop_tess9(void *s, const char *sel, uint64_t a, uint64_t b, uint64_t c, void *d, uint64_t e, void *f, uint64_t g, uint64_t h, uint64_t i) {
    (void)s; (void)sel; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
}
static void mtl_re_noop_setTessBuf(void *s, const char *sel, void *b, uint64_t off, uint64_t stride) {
    (void)s; (void)sel; (void)b; (void)off; (void)stride;
}
static void mtl_re_noop_flt(void *s, const char *sel, float f) { (void)s; (void)sel; (void)f; }
static void *mtl_re_self(void *s, const char *sel) { (void)sel; return s; }

/* ============================================================================
 * MTLComputeCommandEncoder
 * ============================================================================ */
static void mtl_ce_dealloc(void *self) {
    get_state().live_encoders.fetch_sub(1, std::memory_order_relaxed);
    delete unwrap_mtl<MtlComputeEncoderImpl>(self);
}

static void *mtl_cb_computeEncoder(void *self, const char *sel) {
    (void)sel;
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(self);
    if (!cb) return nullptr;
    check_begin_cb(cb);
    auto *ce = new MtlComputeEncoderImpl();
    ce->cmd_buf_obj = self;
    ce->cmd_buf = cb;
    get_state().live_encoders.fetch_add(1, std::memory_order_relaxed);
    return alloc_mtl_wrap_managed("MTLComputeCommandEncoder", ce, &mtl_ce_dealloc, true);
}

static void *mtl_cb_computeEncoderType(void *self, const char *sel, uint64_t dispatchType) {
    (void)dispatchType;
    return mtl_cb_computeEncoder(self, sel);
}

static void mtl_ce_setPSO(void *s, const char *sel, void *cpso) { (void)sel; if (auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s)) ce->pipeline_state_obj = cpso; }
static void mtl_ce_setBuf(void *s, const char *sel, void *b, uint64_t off, uint64_t idx) { (void)sel; if (auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s)) bind_stage_buffer(ce->cs, idx, b, off); }
static void mtl_ce_setBufOff(void *s, const char *sel, uint64_t off, uint64_t idx) {
    (void)sel;
    if (auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s)) set_stage_buffer_offset(ce->cs, idx, off);
}
static void mtl_ce_setBytes(void *s, const char *sel, const void *ptr, uint64_t len, uint64_t idx) { (void)sel; if (auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s)) bind_stage_bytes(ce->cmd_buf, ce->cs, ptr, len, idx); }
static void mtl_ce_setTex(void *s, const char *sel, void *t, uint64_t idx) { (void)sel; if (auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s)) bind_stage_texture(ce->cmd_buf, ce->cs, idx, t, false); }
static void mtl_ce_setSmp(void *s, const char *sel, void *smp, uint64_t idx) { (void)sel; if (auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s)) bind_stage_sampler(ce->cs, idx, smp); }

static void mtl_ce_dispatchTG(void *s, const char *sel, MTLSize *tgs, MTLSize *tptg) {
    (void)sel; (void)tptg;
    auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s);
    if (!ce || !check_begin_cb(ce->cmd_buf) || !ce->pipeline_state_obj || !tgs) return;
    mtl_flush_pending_tex_uploads_cmd(ce->cmd_buf);
    auto *cpso = unwrap_mtl<MtlComputePipelineStateImpl>(ce->pipeline_state_obj);
    if (!cpso || cpso->pipeline == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = ce->cmd_buf->cmd;
    for (uint64_t k = 0; k < kMaxTextures; ++k) {
        if (!ce->cs.tex_bound[k] || !ce->cs.tex_obj[k]) continue;
        auto *t = unwrap_mtl<MtlTextureImpl>(ce->cs.tex_obj[k]);
        if (!t || t->image == VK_NULL_HANDLE) continue;
        if (t->parent_buffer_obj) {
            auto *buf = unwrap_mtl<MtlBufferImpl>(t->parent_buffer_obj);
            if (buf && buf->buffer != VK_NULL_HANDLE) {
                uint64_t bgen = buf->mod_gen.load(std::memory_order_relaxed);
                if (t->synced_cmd_seq != ce->cmd_buf->cmd_seq_id || t->synced_buf_gen != bgen) {
                    record_buffer_backed_texture_copy(cmd, t, buf);
                    t->synced_cmd_seq = ce->cmd_buf->cmd_seq_id;
                    t->synced_buf_gen = bgen;
                }
            }
        } else if (!t->mip_layouts.empty() &&
                   t->mip_layouts[0] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            transition_image_layout(cmd, t->image, t->aspect_mask, 0, t->mip_levels, 0, t->array_layers,
                                    t->mip_layouts[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            for (auto &ml : t->mip_layouts) ml = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpso->pipeline);
    VkDescriptorSet set0 = allocate_and_write_set(ce->cmd_buf, cpso->set0_layout, cpso->cs, ce->cs);
    if (set0 != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpso->pipeline_layout, 0, 1, &set0, 0, nullptr);
    }

    uint32_t gx = (uint32_t)std::max<uint64_t>(1, tgs->width);
    uint32_t gy = (uint32_t)std::max<uint64_t>(1, tgs->height);
    uint32_t gz = (uint32_t)std::max<uint64_t>(1, tgs->depth);
    vkCmdDispatch(cmd, gx, gy, gz);

    for (uint32_t b = 0; b < kMaxBuffers; ++b) {
        if (ce->cs.buffer_bound[b] && ce->cs.buffer_obj[b]) {
            if (auto *bi = unwrap_mtl<MtlBufferImpl>(ce->cs.buffer_obj[b])) {
                bi->mod_gen.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    get_state().compute_dispatches.fetch_add(1, std::memory_order_relaxed);
}

static void mtl_ce_setBufs(void *s, const char *sel, void *const *bufs, const uint64_t *offs, NSRange64 r) {
    (void)sel;
    auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s);
    if (!ce || !bufs) return;
    for (uint64_t i = 0; i < r.length; ++i) bind_stage_buffer(ce->cs, r.location + i, bufs[i], offs ? offs[i] : 0ULL);
}
static void mtl_ce_setTexs(void *s, const char *sel, void *const *texs, NSRange64 r) {
    (void)sel;
    auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s);
    if (!ce || !texs) return;
    for (uint64_t i = 0; i < r.length; ++i) bind_stage_texture(ce->cmd_buf, ce->cs, r.location + i, texs[i], false);
}
static void mtl_ce_setSmps(void *s, const char *sel, void *const *smps, NSRange64 r) {
    (void)sel;
    auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s);
    if (!ce || !smps) return;
    for (uint64_t i = 0; i < r.length; ++i) bind_stage_sampler(ce->cs, r.location + i, smps[i]);
}
static void mtl_ce_setSmpClamps(void *s, const char *sel, void *smp, float lmin, float lmax, uint64_t idx) {
    (void)lmin; (void)lmax; mtl_ce_setSmp(s, sel, smp, idx);
}
static void mtl_ce_setSmpsClamps(void *s, const char *sel, void *const *smps, const float *lmin, const float *lmax, NSRange64 r) {
    (void)lmin; (void)lmax; mtl_ce_setSmps(s, sel, smps, r);
}
static void mtl_ce_dispatchIndirect(void *s, const char *sel, void *ind_obj, uint64_t ind_off, MTLSize *tptg) {
    (void)sel; (void)tptg;
    auto *ce = unwrap_mtl<MtlComputeEncoderImpl>(s);
    auto *ind = unwrap_mtl<MtlBufferImpl>(ind_obj);
    if (!ce || !check_begin_cb(ce->cmd_buf) || !ce->pipeline_state_obj || !ind || ind->buffer == VK_NULL_HANDLE) return;
    auto *cpso = unwrap_mtl<MtlComputePipelineStateImpl>(ce->pipeline_state_obj);
    if (!cpso || cpso->pipeline == VK_NULL_HANDLE) return;
    VkCommandBuffer cmd = ce->cmd_buf->cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpso->pipeline);
    VkDescriptorSet set0 = allocate_and_write_set(ce->cmd_buf, cpso->set0_layout, cpso->cs, ce->cs);
    if (set0 != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpso->pipeline_layout, 0, 1, &set0, 0, nullptr);
    }
    vkCmdDispatchIndirect(cmd, ind->buffer, ind_off);
    get_state().compute_dispatches.fetch_add(1, std::memory_order_relaxed);
}

static void mtl_ce_dispatchThreads(void *s, const char *sel, MTLSize *threads, MTLSize *tptg) {
    if (!threads || !tptg) return;
    MTLSize tgs{
        (threads->width + std::max<uint64_t>(1, tptg->width) - 1) / std::max<uint64_t>(1, tptg->width),
        (threads->height + std::max<uint64_t>(1, tptg->height) - 1) / std::max<uint64_t>(1, tptg->height),
        (threads->depth + std::max<uint64_t>(1, tptg->depth) - 1) / std::max<uint64_t>(1, tptg->depth),
    };
    mtl_ce_dispatchTG(s, sel, &tgs, tptg);
}

/* ============================================================================
 * MTLBlitCommandEncoder
 * ============================================================================ */
static void mtl_be_dealloc(void *self) {
    get_state().live_encoders.fetch_sub(1, std::memory_order_relaxed);
    delete unwrap_mtl<MtlBlitEncoderImpl>(self);
}

static void *mtl_cb_blitEncoder(void *self, const char *sel) {
    (void)sel;
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(self);
    if (!cb) return nullptr;
    if (check_begin_cb(cb)) {
        mtl_flush_pending_tex_uploads_cmd(cb);
    }
    auto *be = new MtlBlitEncoderImpl();
    be->cmd_buf_obj = self;
    be->cmd_buf = cb;
    get_state().live_encoders.fetch_add(1, std::memory_order_relaxed);
    return alloc_mtl_wrap_managed("MTLBlitCommandEncoder", be, &mtl_be_dealloc, true);
}

static void mtl_be_copyBuf(void *s, const char *sel, void *src_obj, uint64_t src_off,
                           void *dst_obj, uint64_t dst_off, uint64_t size) {
    (void)sel;
    auto *be = unwrap_mtl<MtlBlitEncoderImpl>(s);
    auto *src = unwrap_mtl<MtlBufferImpl>(src_obj);
    auto *dst = unwrap_mtl<MtlBufferImpl>(dst_obj);
    if (!be || !check_begin_cb(be->cmd_buf) || !src || !dst || size == 0) return;
    VkBufferCopy reg{src_off, dst_off, size};
    vkCmdCopyBuffer(be->cmd_buf->cmd, src->buffer, dst->buffer, 1, &reg);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                       VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                       VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(be->cmd_buf->cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    dst->mod_gen.fetch_add(1, std::memory_order_relaxed);
    for (MtlTextureImpl *vt : dst->buffer_views) {
        if (vt && vt->buffer_offset < dst_off + size) {
            record_buffer_backed_texture_copy(be->cmd_buf->cmd, vt, dst);
        }
    }
    get_state().blit_commands.fetch_add(1, std::memory_order_relaxed);
}

static void mtl_be_fillBuf(void *s, const char *sel, void *buf_obj, NSRange64 range, uint8_t val) {
    (void)sel;
    auto *be = unwrap_mtl<MtlBlitEncoderImpl>(s);
    auto *buf = unwrap_mtl<MtlBufferImpl>(buf_obj);
    if (!be || !check_begin_cb(be->cmd_buf) || !buf || range.length == 0) return;
    uint32_t fill32 = (uint32_t)val | ((uint32_t)val << 8) | ((uint32_t)val << 16) | ((uint32_t)val << 24);
    vkCmdFillBuffer(be->cmd_buf->cmd, buf->buffer, range.location, range.length, fill32);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                       VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                       VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(be->cmd_buf->cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    buf->mod_gen.fetch_add(1, std::memory_order_relaxed);
    get_state().blit_commands.fetch_add(1, std::memory_order_relaxed);
}

static void mtl_be_copyTexToBuf(void *s, const char *sel, void *src_obj, uint64_t src_slc, uint64_t src_lvl,
                                MTLOrigin *src_org, MTLSize *src_sz, void *dst_obj, uint64_t dst_off,
                                uint64_t dst_bpr, uint64_t dst_bpi, uint64_t opts) {
    (void)sel; (void)dst_bpr; (void)dst_bpi; (void)opts;
    auto *be = unwrap_mtl<MtlBlitEncoderImpl>(s);
    auto *tex = unwrap_mtl<MtlTextureImpl>(src_obj);
    auto *buf = unwrap_mtl<MtlBufferImpl>(dst_obj);
    if (!be || !check_begin_cb(be->cmd_buf) || !tex || !buf || !src_org || !src_sz) return;

    VkCommandBuffer cmd = be->cmd_buf->cmd;
    uint32_t lvl = (uint32_t)src_lvl;
    uint32_t slc = (uint32_t)src_slc;
    VkImageLayout old_l = tex->get_subres_layout(lvl, slc);
    transition_image_layout(cmd, tex->image, tex->aspect_mask, lvl, 1, slc, 1, old_l, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy bic{};
    bic.bufferOffset = dst_off;
    bic.imageSubresource = {tex->aspect_mask, lvl, slc, 1};
    bic.imageOffset = {(int32_t)src_org->x, (int32_t)src_org->y, (int32_t)src_org->z};
    bic.imageExtent = {(uint32_t)src_sz->width, (uint32_t)src_sz->height, (uint32_t)std::max<uint64_t>(1, src_sz->depth)};
    vkCmdCopyImageToBuffer(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf->buffer, 1, &bic);
    buf->mod_gen.fetch_add(1, std::memory_order_relaxed);

    VkImageLayout restore_l = (old_l != VK_IMAGE_LAYOUT_UNDEFINED) ? old_l : VK_IMAGE_LAYOUT_GENERAL;
    transition_image_layout(cmd, tex->image, tex->aspect_mask, lvl, 1, slc, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, restore_l);
    tex->set_subres_layout(lvl, slc, restore_l);
    get_state().blit_commands.fetch_add(1, std::memory_order_relaxed);
}

static void mtl_be_copyTexToBuf9(void *s, const char *sel, void *src_obj, uint64_t src_slc, uint64_t src_lvl,
                                 MTLOrigin *src_org, MTLSize *src_sz, void *dst_obj, uint64_t dst_off,
                                 uint64_t dst_bpr, uint64_t dst_bpi) {
    mtl_be_copyTexToBuf(s, sel, src_obj, src_slc, src_lvl, src_org, src_sz, dst_obj, dst_off, dst_bpr, dst_bpi, 0);
}

static void mtl_be_copyBufToTex(void *s, const char *sel, void *src_obj, uint64_t src_off, uint64_t src_bpr,
                                uint64_t src_bpi, const MTLSize *src_sz, void *dst_obj, uint64_t dst_slc,
                                uint64_t dst_lvl, const MTLOrigin *dst_org, uint64_t opts) {
    (void)sel;
    auto *be = unwrap_mtl<MtlBlitEncoderImpl>(s);
    auto *src = unwrap_mtl<MtlBufferImpl>(src_obj);
    auto *dst = unwrap_mtl<MtlTextureImpl>(dst_obj);
    if (!be || !check_begin_cb(be->cmd_buf) || !src || src->buffer == VK_NULL_HANDLE ||
        !dst || dst->image == VK_NULL_HANDLE || !src_sz || !dst_org) return;

    uint32_t lvl = (dst->mip_levels > 0) ? (uint32_t)std::min<uint64_t>(dst_lvl, dst->mip_levels - 1) : 0;
    uint32_t slc = (dst->array_layers > 0) ? (uint32_t)std::min<uint64_t>(dst_slc, dst->array_layers - 1) : 0;
    uint32_t mip_w = std::max(1u, dst->width >> lvl);
    uint32_t mip_h = std::max(1u, dst->height >> lvl);
    uint32_t mip_d = std::max(1u, dst->depth >> lvl);

    int32_t ox = (int32_t)std::min<uint64_t>(dst_org->x, mip_w - 1);
    int32_t oy = (int32_t)std::min<uint64_t>(dst_org->y, mip_h - 1);
    int32_t oz = (int32_t)std::min<uint64_t>(dst_org->z, mip_d - 1);
    uint32_t cw = std::min((uint32_t)std::max<uint64_t>(1, src_sz->width), mip_w - (uint32_t)ox);
    uint32_t ch = std::min((uint32_t)std::max<uint64_t>(1, src_sz->height), mip_h - (uint32_t)oy);
    uint32_t cd = std::min((uint32_t)std::max<uint64_t>(1, src_sz->depth), mip_d - (uint32_t)oz);

    uint32_t bw = 1, bh = 1;
    uint32_t bsz = mtl_pixel_format_block_size(dst->vk_format, &bw, &bh);
    uint32_t buf_row_len = 0;
    if (src_bpr > 0 && bsz > 0) {
        uint64_t blocks_row = src_bpr / bsz;
        uint64_t row_texels = blocks_row * bw;
        if (row_texels >= cw && row_texels <= 65536ULL) {
            buf_row_len = (uint32_t)row_texels;
        }
    }
    uint32_t buf_img_h = 0;
    if (cd > 1 && src_bpi > 0 && src_bpr > 0) {
        uint64_t rows_img = src_bpi / src_bpr;
        uint64_t h_texels = rows_img * bh;
        if (h_texels >= ch && h_texels <= 65536ULL) {
            buf_img_h = (uint32_t)h_texels;
        }
    }

    VkImageAspectFlags copy_aspect = dst->aspect_mask;
    if (copy_aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        copy_aspect = (opts & 0x02ULL) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    VkCommandBuffer cmd = be->cmd_buf->cmd;
    VkImageLayout old_l = dst->get_subres_layout(lvl, slc);
    transition_image_layout(cmd, dst->image, dst->aspect_mask, lvl, 1, slc, 1, old_l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy bic{};
    bic.bufferOffset = (bsz >= 4) ? static_cast<VkDeviceSize>(src_off & ~static_cast<uint64_t>(bsz - 1)) : static_cast<VkDeviceSize>(src_off);
    bic.bufferRowLength = buf_row_len;
    bic.bufferImageHeight = buf_img_h;
    bic.imageSubresource = {copy_aspect, lvl, slc, 1};
    bic.imageOffset = {ox, oy, oz};
    bic.imageExtent = {cw, ch, cd};
    vkCmdCopyBufferToImage(cmd, src->buffer, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

    VkImageLayout restore_l = dst->is_swapchain ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition_image_layout(cmd, dst->image, dst->aspect_mask, lvl, 1, slc, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, restore_l);
    dst->set_subres_layout(lvl, slc, restore_l);
    get_state().blit_commands.fetch_add(1, std::memory_order_relaxed);
}

static void mtl_be_copyBufToTex9(void *s, const char *sel, void *src_obj, uint64_t src_off, uint64_t src_bpr,
                                 uint64_t src_bpi, const MTLSize *src_sz, void *dst_obj, uint64_t dst_slc,
                                 uint64_t dst_lvl, const MTLOrigin *dst_org) {
    mtl_be_copyBufToTex(s, sel, src_obj, src_off, src_bpr, src_bpi, src_sz, dst_obj, dst_slc, dst_lvl, dst_org, 0);
}

static void mtl_be_copyTexToTex(void *s, const char *sel, void *src_obj, uint64_t src_slc, uint64_t src_lvl,
                                MTLOrigin *src_org, MTLSize *src_sz, void *dst_obj, uint64_t dst_slc,
                                uint64_t dst_lvl, MTLOrigin *dst_org) {
    (void)sel;
    auto *be = unwrap_mtl<MtlBlitEncoderImpl>(s);
    auto *stex = unwrap_mtl<MtlTextureImpl>(src_obj);
    auto *dtex = unwrap_mtl<MtlTextureImpl>(dst_obj);
    if (!be || !check_begin_cb(be->cmd_buf) || !stex || !dtex || !src_org || !src_sz || !dst_org) return;

    VkCommandBuffer cmd = be->cmd_buf->cmd;
    VkImageLayout sold = stex->get_subres_layout((uint32_t)src_lvl, (uint32_t)src_slc);
    VkImageLayout dold = dtex->get_subres_layout((uint32_t)dst_lvl, (uint32_t)dst_slc);
    uint32_t dmip_w = std::max(1u, dtex->width >> (uint32_t)dst_lvl);
    uint32_t dmip_h = std::max(1u, dtex->height >> (uint32_t)dst_lvl);
    bool full_overwrite = (dst_org->x == 0 && dst_org->y == 0 && dst_org->z == 0 &&
                           src_sz->width >= dmip_w && src_sz->height >= dmip_h);
    transition_image_layout(cmd, stex->image, stex->aspect_mask, (uint32_t)src_lvl, 1, (uint32_t)src_slc, 1, sold, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition_image_layout(cmd, dtex->image, dtex->aspect_mask, (uint32_t)dst_lvl, 1, (uint32_t)dst_slc, 1,
                            full_overwrite ? VK_IMAGE_LAYOUT_UNDEFINED : dold, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy ic{};
    ic.srcSubresource = {stex->aspect_mask, (uint32_t)src_lvl, (uint32_t)src_slc, 1};
    ic.srcOffset = {(int32_t)src_org->x, (int32_t)src_org->y, (int32_t)src_org->z};
    ic.dstSubresource = {dtex->aspect_mask, (uint32_t)dst_lvl, (uint32_t)dst_slc, 1};
    ic.dstOffset = {(int32_t)dst_org->x, (int32_t)dst_org->y, (int32_t)dst_org->z};
    ic.extent = {(uint32_t)src_sz->width, (uint32_t)src_sz->height, (uint32_t)std::max<uint64_t>(1, src_sz->depth)};
    vkCmdCopyImage(cmd, stex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dtex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);

    VkImageLayout srest = (sold != VK_IMAGE_LAYOUT_UNDEFINED) ? sold : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageLayout drest = dtex->is_swapchain ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition_image_layout(cmd, stex->image, stex->aspect_mask, (uint32_t)src_lvl, 1, (uint32_t)src_slc, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srest);
    transition_image_layout(cmd, dtex->image, dtex->aspect_mask, (uint32_t)dst_lvl, 1, (uint32_t)dst_slc, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, drest);
    stex->set_subres_layout((uint32_t)src_lvl, (uint32_t)src_slc, srest);
    dtex->set_subres_layout((uint32_t)dst_lvl, (uint32_t)dst_slc, drest);
    get_state().blit_commands.fetch_add(1, std::memory_order_relaxed);
}

static void mtl_be_genMipmaps(void *s, const char *sel, void *tex_obj) {
    (void)sel;
    auto *be = unwrap_mtl<MtlBlitEncoderImpl>(s);
    auto *tex = unwrap_mtl<MtlTextureImpl>(tex_obj);
    if (!be || !check_begin_cb(be->cmd_buf) || !tex || tex->image == VK_NULL_HANDLE || tex->mip_levels <= 1) return;

    VkCommandBuffer cmd = be->cmd_buf->cmd;
    VkImageLayout l0 = tex->get_subres_layout(0, 0);
    if (l0 == VK_IMAGE_LAYOUT_UNDEFINED) {
        for (uint32_t m = 0; m < tex->mip_levels; ++m) {
            transition_image_layout(cmd, tex->image, tex->aspect_mask, m, 1, 0, tex->array_layers,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            for (uint32_t a = 0; a < tex->array_layers; ++a) {
                tex->set_subres_layout(m, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
        return;
    }

    transition_image_layout(cmd, tex->image, tex->aspect_mask, 0, 1, 0, tex->array_layers,
                            l0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    for (uint32_t m = 1; m < tex->mip_levels; ++m) {
        VkImageLayout lm = tex->get_subres_layout(m, 0);
        transition_image_layout(cmd, tex->image, tex->aspect_mask, m, 1, 0, tex->array_layers,
                                lm, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        int32_t sw = (int32_t)std::max(1u, tex->width >> (m - 1));
        int32_t sh = (int32_t)std::max(1u, tex->height >> (m - 1));
        int32_t sd = (int32_t)std::max(1u, tex->depth >> (m - 1));
        int32_t dw = (int32_t)std::max(1u, tex->width >> m);
        int32_t dh = (int32_t)std::max(1u, tex->height >> m);
        int32_t dd = (int32_t)std::max(1u, tex->depth >> m);

        VkImageBlit ib{};
        ib.srcSubresource = {tex->aspect_mask, m - 1, 0, tex->array_layers};
        ib.srcOffsets[1] = {sw, sh, sd};
        ib.dstSubresource = {tex->aspect_mask, m, 0, tex->array_layers};
        ib.dstOffsets[1] = {dw, dh, dd};
        vkCmdBlitImage(cmd,
                       tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &ib, VK_FILTER_LINEAR);

        transition_image_layout(cmd, tex->image, tex->aspect_mask, m - 1, 1, 0, tex->array_layers,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (uint32_t a = 0; a < tex->array_layers; ++a) {
            tex->set_subres_layout(m - 1, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        if (m + 1 < tex->mip_levels) {
            transition_image_layout(cmd, tex->image, tex->aspect_mask, m, 1, 0, tex->array_layers,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        } else {
            transition_image_layout(cmd, tex->image, tex->aspect_mask, m, 1, 0, tex->array_layers,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            for (uint32_t a = 0; a < tex->array_layers; ++a) {
                tex->set_subres_layout(m, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }
    get_state().blit_commands.fetch_add(1, std::memory_order_relaxed);
}

static void mtl_be_syncTex(void *s, const char *sel, void *tex_obj, uint64_t slc, uint64_t lvl) {
    (void)s; (void)sel; (void)tex_obj; (void)slc; (void)lvl;
}

} // namespace

void register_encoder_objc_methods() {
    oh2_objc_ensure_class("MTLRenderCommandEncoder", "NSObject");
    oh2_objc_ensure_class("MTLComputeCommandEncoder", "NSObject");
    oh2_objc_ensure_class("MTLBlitCommandEncoder", "NSObject");

    oh2_objc_register_instance_method("MTLCommandBuffer", "renderCommandEncoderWithDescriptor:", (void *)&mtl_cb_renderEncoder);
    oh2_objc_register_instance_method("MTLCommandBuffer", "computeCommandEncoder",               (void *)&mtl_cb_computeEncoder);
    oh2_objc_register_instance_method("MTLCommandBuffer", "computeCommandEncoderWithDispatchType:", (void *)&mtl_cb_computeEncoderType);
    oh2_objc_register_instance_method("MTLCommandBuffer", "blitCommandEncoder",                  (void *)&mtl_cb_blitEncoder);

    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setRenderPipelineState:",       (void *)&mtl_re_setPSO);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setDepthStencilState:",         (void *)&mtl_re_setDSS);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setCullMode:",                  (void *)&mtl_re_setCull);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFrontFacingWinding:",        (void *)&mtl_re_setWind);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTriangleFillMode:",          (void *)&mtl_re_setFill);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setStencilReferenceValue:",     (void *)&mtl_re_setStencilRef);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setStencilFrontReferenceValue:backReferenceValue:", (void *)&mtl_re_setStencilFB);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setDepthBias:slopeScale:clamp:", (void *)&mtl_re_setDepthBias);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setViewport:",                  (void *)&mtl_re_setViewport);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setViewports:count:",           (void *)&mtl_re_setViewports);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setScissorRect:",               (void *)&mtl_re_setScissor);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setScissorRects:count:",        (void *)&mtl_re_setScissors);

    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexBuffer:offset:atIndex:",       (void *)&mtl_re_setVB);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexBufferOffset:atIndex:",        (void *)&mtl_re_setVBO);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexBytes:length:atIndex:",        (void *)&mtl_re_setVBytes);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexTexture:atIndex:",             (void *)&mtl_re_setVT);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexSamplerState:atIndex:",        (void *)&mtl_re_setVS);

    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentBuffer:offset:atIndex:",     (void *)&mtl_re_setFB);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentBufferOffset:atIndex:",      (void *)&mtl_re_setFBO);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentBytes:length:atIndex:",      (void *)&mtl_re_setFBytes);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentTexture:atIndex:",           (void *)&mtl_re_setFT);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentSamplerState:atIndex:",      (void *)&mtl_re_setFS);

    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawPrimitives:vertexStart:vertexCount:", (void *)&mtl_re_draw);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawPrimitives:vertexStart:vertexCount:instanceCount:", (void *)&mtl_re_drawInst);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:", (void *)&mtl_re_drawInstBase);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", (void *)&mtl_re_drawIdx);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:", (void *)&mtl_re_drawIdxInst);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:", (void *)&mtl_re_drawIdxFull);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "endEncoding",                   (void *)&mtl_re_endEncoding);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "device",                        (void *)&mtl_enc_device);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "label",                         (void *)&mtl_enc_label);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setLabel:",                     (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "insertDebugSignpost:",          (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "pushDebugGroup:",               (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "popDebugGroup",                 (void *)&mtl_enc_noop);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexBuffers:offsets:withRange:", (void *)&mtl_re_setVBs);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexTextures:withRange:",        (void *)&mtl_re_setVTs);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexSamplerStates:withRange:",   (void *)&mtl_re_setVSs);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:", (void *)&mtl_re_setVSClamps);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVertexSamplerStates:lodMinClamps:lodMaxClamps:withRange:", (void *)&mtl_re_setVSsClamps);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentBuffers:offsets:withRange:", (void *)&mtl_re_setFBs);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentTextures:withRange:",        (void *)&mtl_re_setFTs);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentSamplerStates:withRange:",   (void *)&mtl_re_setFSs);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:", (void *)&mtl_re_setFSClamps);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setFragmentSamplerStates:lodMinClamps:lodMaxClamps:withRange:", (void *)&mtl_re_setFSsClamps);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setDepthClipMode:",             (void *)&mtl_re_setDepthClipMode);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setBlendColorRed:green:blue:alpha:", (void *)&mtl_re_setBlendColor);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setVisibilityResultMode:offset:", (void *)&mtl_re_setVisResultMode);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setDepthStoreAction:",          (void *)&mtl_re_setDepthStoreAction);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setStencilStoreAction:",        (void *)&mtl_re_setStencilStoreAction);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setDepthStoreActionOptions:",   (void *)&mtl_enc_noop_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setStencilStoreActionOptions:", (void *)&mtl_enc_noop_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setColorStoreAction:atIndex:",  (void *)&mtl_re_setColorStoreAction);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setColorStoreActionOptions:atIndex:", (void *)&mtl_enc_noop_u64_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawPrimitives:indirectBuffer:indirectBufferOffset:", (void *)&mtl_re_drawIndirect);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:", (void *)&mtl_re_drawIdxIndirect);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "textureBarrier",                (void *)&mtl_enc_noop);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "updateFence:afterStages:",      (void *)&mtl_enc_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "waitForFence:beforeStages:",    (void *)&mtl_enc_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "useResource:usage:",            (void *)&mtl_enc_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "useResources:count:usage:",     (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "useHeap:",                      (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "useHeaps:count:",               (void *)&mtl_enc_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "memoryBarrierWithResources:count:", (void *)&mtl_enc_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTessellationFactorBuffer:offset:instanceStride:", (void *)&mtl_re_noop_setTessBuf);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTessellationFactorScale:",   (void *)&mtl_re_noop_flt);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:instanceCount:baseInstance:", (void *)&mtl_re_noop_tess);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:", (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:baseInstance:", (void *)&mtl_re_noop_tess9);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:", (void *)&mtl_re_noop_tess);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileBytes:length:atIndex:",  (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileBuffer:offset:atIndex:", (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileBufferOffset:atIndex:",  (void *)&mtl_enc_noop_u64_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileBuffers:offsets:withRange:", (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileTexture:atIndex:",       (void *)&mtl_enc_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileTextures:withRange:",    (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileSamplerState:atIndex:",  (void *)&mtl_enc_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileSamplerStates:withRange:", (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileSamplerState:lodMinClamp:lodMaxClamp:atIndex:", (void *)&mtl_re_setFSClamps);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setTileSamplerStates:lodMinClamps:lodMaxClamps:withRange:", (void *)&mtl_re_setFSsClamps);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "dispatchThreadsPerTile:",       (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "tileWidth",                     (void *)&mtl_enc_zero);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "tileHeight",                    (void *)&mtl_enc_zero);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "setThreadgroupMemoryLength:offset:atIndex:", (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLRenderCommandEncoder", "renderCommandEncoder",          (void *)&mtl_re_self);

    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setComputePipelineState:",     (void *)&mtl_ce_setPSO);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setBuffer:offset:atIndex:",    (void *)&mtl_ce_setBuf);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setBufferOffset:atIndex:",     (void *)&mtl_ce_setBufOff);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setBuffers:offsets:withRange:",(void *)&mtl_ce_setBufs);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setBytes:length:atIndex:",     (void *)&mtl_ce_setBytes);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setTexture:atIndex:",          (void *)&mtl_ce_setTex);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setTextures:withRange:",       (void *)&mtl_ce_setTexs);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setSamplerState:atIndex:",     (void *)&mtl_ce_setSmp);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setSamplerStates:withRange:",  (void *)&mtl_ce_setSmps);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setSamplerState:lodMinClamp:lodMaxClamp:atIndex:", (void *)&mtl_ce_setSmpClamps);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setSamplerStates:lodMinClamps:lodMaxClamps:withRange:", (void *)&mtl_ce_setSmpsClamps);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setThreadgroupMemoryLength:atIndex:", (void *)&mtl_enc_noop_u64_u64);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setStageInRegion:",            (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setImageblockWidth:height:",   (void *)&mtl_enc_noop_u64_u64);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "dispatchThreadgroups:threadsPerThreadgroup:", (void *)&mtl_ce_dispatchTG);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "dispatchThreads:threadsPerThreadgroup:",      (void *)&mtl_ce_dispatchThreads);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:", (void *)&mtl_ce_dispatchIndirect);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "endEncoding",                  (void *)&mtl_enc_noop);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "device",                       (void *)&mtl_enc_device);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "label",                        (void *)&mtl_enc_label);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "setLabel:",                    (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "insertDebugSignpost:",         (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "pushDebugGroup:",              (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "popDebugGroup",                (void *)&mtl_enc_noop);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "updateFence:",                 (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "waitForFence:",                (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "useResource:usage:",           (void *)&mtl_enc_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "useResources:count:usage:",    (void *)&mtl_enc_noop_ptr_cnt_u64);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "useHeap:",                     (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLComputeCommandEncoder", "useHeaps:count:",              (void *)&mtl_enc_noop_ptr_u64);

    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "device",                          (void *)&mtl_enc_device);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "label",                           (void *)&mtl_enc_label);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "setLabel:",                       (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "insertDebugSignpost:",            (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "pushDebugGroup:",                 (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "popDebugGroup",                   (void *)&mtl_enc_noop);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "updateFence:",                    (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "waitForFence:",                   (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:", (void *)&mtl_be_copyBuf);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:", (void *)&mtl_be_copyBufToTex9);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:options:", (void *)&mtl_be_copyBufToTex);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "fillBuffer:range:value:",         (void *)&mtl_be_fillBuf);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:", (void *)&mtl_be_copyTexToBuf9);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:", (void *)&mtl_be_copyTexToBuf);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:", (void *)&mtl_be_copyTexToTex);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "generateMipmapsForTexture:",      (void *)&mtl_be_genMipmaps);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "optimizeContentsForGPUAccess:",   (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "optimizeContentsForCPUAccess:",   (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "optimizeContentsForGPUAccess:slice:level:", (void *)&mtl_enc_noop_ptr_slc);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "optimizeContentsForCPUAccess:slice:level:", (void *)&mtl_enc_noop_ptr_slc);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "synchronizeResource:",            (void *)&mtl_enc_setLabel);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "synchronizeTexture:slice:level:", (void *)&mtl_be_syncTex);
    oh2_objc_register_instance_method("MTLBlitCommandEncoder", "endEncoding",                     (void *)&mtl_enc_noop);
}

} // namespace oh2::mtlvk
