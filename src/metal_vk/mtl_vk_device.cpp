#include "mtl_vk_internal.h"
#include "../libc_wrapper/libc_wrapper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

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

/* Dynamically loaded Android ADPF (APerformanceHintManager, API 33+) + ANativeWindow_setFrameRate (API 30+)
   + big-core affinity for RenderThread, dynamically synced to 30 FPS vs 60 FPS target to prevent thermal throttling. */
struct AdpfHelper {
    using GetMgrFn       = void *(*)(void);
    using CreateSesFn    = void *(*)(void *, const int32_t *, size_t, int64_t);
    using ReportFn       = int (*)(void *, int64_t);
    using UpdateTargetFn = int (*)(void *, int64_t);
    using SetFrameRateFn = int32_t (*)(ANativeWindow *, float, int8_t);

    void          *session = nullptr;
    ReportFn       report_fn = nullptr;
    UpdateTargetFn update_target_fn = nullptr;
    SetFrameRateFn set_frame_rate_fn = nullptr;
    pid_t          boosted_tid = 0;
    int            last_target_fps = 0;
    ANativeWindow *last_rate_win = nullptr;
    uint64_t       frame_accum_work_ns = 0;

    void boost_render_thread_once() {
        pid_t tid = gettid();
        if (__builtin_expect(boosted_tid == tid, 1)) return;
        boosted_tid = tid;

        /* 1. Detect big/prime cores (max_freq > min_max_freq) and pin RenderThread off LITTLE cores */
        uint64_t freqs[16]{};
        uint64_t min_f = UINT64_MAX, max_f = 0;
        int ncpus = 0;
        for (int c = 0; c < 16; ++c) {
            char p[128];
            std::snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
            FILE *fp = std::fopen(p, "r");
            if (!fp) break;
            unsigned long long f = 0;
            if (std::fscanf(fp, "%llu", &f) == 1 && f > 0) {
                freqs[c] = (uint64_t)f;
                if (freqs[c] < min_f) min_f = freqs[c];
                if (freqs[c] > max_f) max_f = freqs[c];
            }
            std::fclose(fp);
            ncpus = c + 1;
        }
        int big_cnt = 0;
        if (ncpus > 1 && max_f > min_f) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            for (int c = 0; c < ncpus; ++c) {
                if (freqs[c] > min_f) {
                    CPU_SET(c, &mask);
                    big_cnt++;
                }
            }
            if (big_cnt > 0) {
                sched_setaffinity(0, sizeof(mask), &mask);
            }
        }
        setpriority(PRIO_PROCESS, 0, -8);

        /* 2. Register RenderThread with Android Performance Hint Manager (ADPF) + ANativeWindow_setFrameRate */
        void *lib = dlopen("libandroid.so", RTLD_LAZY | RTLD_LOCAL);
        if (lib) {
            auto get_mgr = reinterpret_cast<GetMgrFn>(dlsym(lib, "APerformanceHint_getManager"));
            auto create_ses = reinterpret_cast<CreateSesFn>(dlsym(lib, "APerformanceHint_createSession"));
            report_fn = reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
            update_target_fn = reinterpret_cast<UpdateTargetFn>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
            set_frame_rate_fn = reinterpret_cast<SetFrameRateFn>(dlsym(lib, "ANativeWindow_setFrameRate"));
            if (get_mgr && create_ses && report_fn) {
                void *mgr = get_mgr();
                if (mgr) {
                    int32_t tids[1] = {static_cast<int32_t>(tid)};
                    /* Always use 16.67ms ADPF target ("race-to-idle"): in 30 FPS mode CADisplayLink ticks
                       only every 33.33ms so finishing work fast and entering deep idle prevents clock oscillation! */
                    session = create_ses(mgr, tids, 1, 16666667LL);
                    last_target_fps = 60;
                }
            }
        }
        MTLVK_LOGI("[M5] RenderThread tid=%d boosted: big_cores=%d/%d (freq %llu..%llu kHz) adpf=%d setFrameRate=%d",
                   (int)tid, big_cnt, ncpus,
                   (unsigned long long)min_f, (unsigned long long)max_f,
                   session ? 1 : 0, set_frame_rate_fn ? 1 : 0);
    }

    void sync_target_fps(ANativeWindow *win, int target_fps) {
        if (target_fps <= 0 || target_fps > 120) target_fps = 60;
        const int norm_fps = (target_fps <= 30) ? 30 : 60;
        if (norm_fps != last_target_fps) {
            last_target_fps = norm_fps;
            if (session && update_target_fn) {
                update_target_fn(session, 16666667LL);
            }
            if (win && set_frame_rate_fn) {
                set_frame_rate_fn(win, 60.0f, 1);
                last_rate_win = win;
            }
            MTLVK_LOGI("[M5] FramePacer synced: target=%d FPS (ADPF=16ms race-to-idle, SurfaceFlinger=60Hz)",
                       norm_fps);
        } else if (win && win != last_rate_win && set_frame_rate_fn) {
            set_frame_rate_fn(win, 60.0f, 1);
            last_rate_win = win;
        }
    }

    void report_work_ns(int64_t ns) {
        if (session && report_fn && ns > 0) {
            report_fn(session, ns);
        }
    }
};
static AdpfHelper g_adpf;
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
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         32768},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         32768},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          32768},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           4096},
        {VK_DESCRIPTOR_TYPE_SAMPLER,                16384},
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

uint64_t next_mtl_resource_uid() {
    static std::atomic<uint64_t> s_uid{100ULL};
    return s_uid.fetch_add(1, std::memory_order_relaxed);
}

void enqueue_deferred_buffer(VkBuffer buf, VmaAllocation alloc, void *mapped, uint64_t capacity) {
    if (buf == VK_NULL_HANDLE && alloc == VK_NULL_HANDLE) return;
    MtlVkState &st = get_state();
    std::lock_guard<std::mutex> lk(st.mutex);
    DeferredDestroy d{};
    d.kind = DeferredDestroy::BUFFER;
    d.buffer = buf;
    d.allocation = alloc;
    d.mapped = mapped;
    d.capacity = capacity;
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
    st.desc_resource_gen.fetch_add(1, std::memory_order_relaxed);
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
                if (!force_all && d.buffer != VK_NULL_HANDLE && d.mapped != nullptr && d.capacity >= 4096ULL && d.capacity <= 1048576ULL) {
                    int bi = (d.capacity <= 4096ULL) ? 0 :
                             (d.capacity <= 16384ULL) ? 1 :
                             (d.capacity <= 65536ULL) ? 2 :
                             (d.capacity <= 262144ULL) ? 3 : 4;
                    bool recycled = false;
                    {
                        std::lock_guard<std::mutex> lk(st.mutex);
                        if (st.buffer_pool[bi].size() < 32u) {
                            st.buffer_pool[bi].push_back({d.buffer, d.allocation, d.mapped, d.capacity});
                            recycled = true;
                        }
                    }
                    if (recycled) break;
                }
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
    auto t_iu0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> qlk(st.queue_mutex);
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
    auto iu_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_iu0).count();
    st.prof_imm_up_us.fetch_add((uint64_t)iu_us, std::memory_order_relaxed);
    st.prof_imm_up_cnt.fetch_add(1, std::memory_order_relaxed);
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

static void completion_thread_fn(MtlVkState *st_ptr) {
    MtlVkState &st = *st_ptr;
    while (true) {
        uint32_t fidx = 0;
        PendingCbCompletion item;
        {
            std::unique_lock<std::mutex> clk(st.completion_mutex);
            st.completion_cv.wait(clk, [&]() {
                return st.completion_stop || !st.completion_queue.empty();
            });
            if (st.completion_stop && st.completion_queue.empty()) {
                break;
            }
            fidx = st.completion_queue.front().first;
            item = std::move(st.completion_queue.front().second);
            st.completion_queue.pop_front();
        }

        FrameSyncSlot &slot = st.frames[fidx];
        vkWaitForFences(st.vk_ctx.device(), 1, &slot.in_flight_fence, VK_TRUE, UINT64_MAX);
        auto now_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
        if (item.submit_ns > 0 && now_ns > item.submit_ns) {
            st.prof_gpu_fence_us.fetch_add((now_ns - item.submit_ns) / 1000ULL, std::memory_order_relaxed);
        }
        if (slot.ts_capturing && slot.ts_query_pool != VK_NULL_HANDLE && slot.ts_query_cursor >= 2) {
            std::vector<uint64_t> ts_vals(slot.ts_query_cursor, 0);
            VkResult tr = vkGetQueryPoolResults(
                st.vk_ctx.device(), slot.ts_query_pool, 0, slot.ts_query_cursor,
                ts_vals.size() * sizeof(uint64_t), ts_vals.data(), sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (tr == VK_SUCCESS) {
                double cb_gpu_ms = (ts_vals[slot.ts_query_cursor - 1] >= ts_vals[0])
                                       ? (double)(ts_vals[slot.ts_query_cursor - 1] - ts_vals[0]) * st.timestamp_period_ns / 1e6
                                       : 0.0;
                double wall_ms = (item.submit_ns > 0 && now_ns > item.submit_ns) ? (double)(now_ns - item.submit_ns) / 1e6 : 0.0;
                double rec_ms = (item.begin_ns > 0 && item.submit_ns > item.begin_ns) ? (double)(item.submit_ns - item.begin_ns) / 1e6 : 0.0;
                MTLVK_LOGI("[GPU_TS F#%llu] CB rec=%.2fms gpu=%.2fms wall=%.2fms passes=%zu (t_beg=%.1f t_sub=%.1f t_end=%.1f)",
                           (unsigned long long)item.present_frame_tag, rec_ms, cb_gpu_ms, wall_ms,
                           slot.ts_passes.size(),
                           (double)(item.begin_ns % 100000000000ULL) / 1e6,
                           (double)(item.submit_ns % 100000000000ULL) / 1e6,
                           (double)(now_ns % 100000000000ULL) / 1e6);
                for (size_t pi = 0; pi < slot.ts_passes.size(); ++pi) {
                    const auto &pr = slot.ts_passes[pi];
                    double pms = (pr.end_q < ts_vals.size() && pr.start_q < ts_vals.size() && ts_vals[pr.end_q] >= ts_vals[pr.start_q])
                                     ? (double)(ts_vals[pr.end_q] - ts_vals[pr.start_q]) * st.timestamp_period_ns / 1e6
                                     : 0.0;
                    MTLVK_LOGI("[GPU_TS F#%llu]   P#%zu %ux%u(c=%u,df=%d) draws=%u fbsnap=%u occ=%u -> %.2fms",
                               (unsigned long long)item.present_frame_tag, pi, pr.w, pr.h, pr.c_cnt, pr.dfmt,
                               pr.draws, pr.fb_snaps, pr.occs, pms);
                }
            }
            slot.ts_capturing = false;
            slot.ts_passes.clear();
        }
        st.staging.notify_fence_completed(slot.in_flight_fence);

        if (auto *cb = unwrap_mtl<MtlCommandBufferImpl>(item.cb_obj)) {
            cb->status = 4; /* Completed */
        }
        for (void *blk : item.blocks) {
            invoke_objc_block_1(blk, item.cb_obj);
            release_objc_block(blk);
        }
        oh2_objc_release_object(item.cb_obj);

        {
            std::lock_guard<std::mutex> clk(st.completion_mutex);
            slot.submitted = false;
            st.slot_in_flight[fidx] = false;
        }
        st.completion_done_cv.notify_all();
    }
}

static void wait_slot_idle(MtlVkState &st, uint32_t fidx) {
    auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> clk(st.completion_mutex);
    st.completion_done_cv.wait(clk, [&]() { return !st.slot_in_flight[fidx]; });
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    st.prof_wait_slot_us.fetch_add((uint64_t)us, std::memory_order_relaxed);
}

static void wait_all_slots_idle(MtlVkState &st) {
    std::unique_lock<std::mutex> clk(st.completion_mutex);
    st.completion_done_cv.wait(clk, [&]() {
        if (!st.completion_queue.empty()) return false;
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (st.slot_in_flight[i]) return false;
        }
        return true;
    });
}

VkCommandBuffer mtl_begin_command_buffer_internal(MtlCommandBufferImpl *cb) {
    MtlVkState &st = get_state();
    if (!st.initialized || !cb) return VK_NULL_HANDLE;
    if (cb->recording && cb->cmd != VK_NULL_HANDLE) return cb->cmd;

    VkDevice dev = st.vk_ctx.device();
    g_adpf.boost_render_thread_once();
    uint32_t fidx = st.next_frame_idx;
    st.next_frame_idx = (st.next_frame_idx + 1) % kMaxFramesInFlight;
    FrameSyncSlot &slot = st.frames[fidx];

    wait_slot_idle(st, fidx);
    tick_deferred_destroys(false);
    vkResetFences(dev, 1, &slot.in_flight_fence);
    vkResetCommandPool(dev, slot.cmd_pool, 0);
    /* Persistent per-slot descriptor cache: only reset descriptor pool when a sampled resource was
       destroyed or the pool nears capacity (eliminates ~1,000 vkAllocate/vkUpdateDescriptorSets/frame!). */
    const uint64_t cur_rgen = st.desc_resource_gen.load(std::memory_order_relaxed);
    if (slot.desc_pool_resource_gen != cur_rgen || slot.desc_sets_allocated > 12000u) {
        vkResetDescriptorPool(dev, slot.desc_pool, 0);
        for (VkDescriptorPool p : slot.overflow_desc_pools) {
            if (p != VK_NULL_HANDLE) vkResetDescriptorPool(dev, p, 0);
        }
        slot.active_desc_pool_idx = 0;
        slot.desc_sets_allocated = 0;
        slot.desc_pool_resource_gen = cur_rgen;
        st.prof_desc_resets.fetch_add(1, std::memory_order_relaxed);
        slot.desc_cache_epoch++;
        if (slot.desc_cache_epoch == 0) {
            std::memset(slot.desc_cache, 0, sizeof(slot.desc_cache));
            slot.desc_cache_epoch = 1;
        }
    }
    slot.inline_cursor = 0;
    slot.staging_end_cursor = 0;
    slot.occ_query_cursor = 0;
    slot.ts_query_cursor = 0;
    slot.ts_passes.clear();
    slot.preamble_btex_list.clear();
    slot.ts_capturing = false;

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.cmd, &begin_info) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    if (slot.occ_query_pool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(slot.cmd, slot.occ_query_pool, 0, kMaxOcclusionQueriesPerCB);
    }
    if (slot.ts_capturing) {
        vkCmdResetQueryPool(slot.cmd, slot.ts_query_pool, 0, kMaxTimestampQueriesPerCB);
        vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.ts_query_pool, slot.ts_query_cursor++);
    }
    /* Synchronize GPU writes from prior command buffers on graphics_queue() with targeted stages
       (avoids full TOP_OF_PIPE/ALL_COMMANDS frontend drain while guaranteeing cache coherency). */
    VkMemoryBarrier init_mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    init_mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    init_mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                            VK_ACCESS_INDEX_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT |
                            VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(slot.cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &init_mb, 0, nullptr, 0, nullptr);
    static std::atomic<uint64_t> s_cmd_seq_gen{1};
    cb->frame_idx = fidx;
    cb->fb_snap_seq = 0;
    cb->cmd_seq_id = s_cmd_seq_gen.fetch_add(1, std::memory_order_relaxed);
    cb->begin_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
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
    }
    slot.inline_cursor = aligned_off + copy_len;
    *out_buf = slot.inline_buf;
    *out_off = aligned_off;
    return true;
}

bool mtl_acquire_swapchain_image_internal(MtlMetalLayerImpl *layer, uint32_t *out_sc_idx, VkSemaphore *out_wait_sem, uint64_t *out_surface_gen) {
    MtlVkState &st = get_state();
    if (!st.initialized || !out_sc_idx || !out_wait_sem) return false;

    VkDevice dev = st.vk_ctx.device();
    /* Bound GPU present queue depth to at most 2 in-flight frames (eliminates 3-4 frame latency/pileup) */
    uint32_t old_pres_fidx = st.present_ring_fidx[st.present_ring_pos];
    if (old_pres_fidx < kMaxFramesInFlight) {
        wait_slot_idle(st, old_pres_fidx);
    }
    /* Drain oldest in-flight slot if needed so recycled acquire semaphore is guaranteed idle on GPU */
    uint32_t fidx = st.next_frame_idx;
    wait_slot_idle(st, fidx);
    tick_deferred_destroys(false);

    while (true) {
        std::unique_lock<std::mutex> slk(st.surface_mutex);
        st.surface_cv.wait(slk, [&]() {
            return st.surface_shutting_down || (!st.surface_paused && st.vk_ctx.swapchain() != VK_NULL_HANDLE);
        });
        if (st.surface_shutting_down) return false;

        VkSemaphore acq_sem = st.acquire_sem_pool[st.next_acquire_sem];
        uint32_t sc_idx = 0;
        auto t_acq0 = std::chrono::steady_clock::now();
        VkResult res = vkAcquireNextImageKHR(
            dev, st.vk_ctx.swapchain(), 100000000ULL /* 100ms so background pause never stalls */,
            acq_sem, VK_NULL_HANDLE, &sc_idx);
        auto acq_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_acq0).count();
        st.prof_acq_us.fetch_add((uint64_t)acq_us, std::memory_order_relaxed);

        if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
            st.next_acquire_sem = (st.next_acquire_sem + 1) % 8;
            *out_sc_idx = sc_idx;
            *out_wait_sem = acq_sem;
            if (out_surface_gen) *out_surface_gen = st.surface_gen;
            (void)layer;
            return true;
        }
        if (res == VK_TIMEOUT || res == VK_NOT_READY) {
            slk.unlock();
            usleep(2000);
            continue;
        }
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_ERROR_SURFACE_LOST_KHR) {
            st.swapchain_out_of_date = true;
            slk.unlock();
            usleep(10000);
            if (st.surface_paused) continue;
            return false;
        }
        MTLVK_LOGE("vkAcquireNextImageKHR failed: %d", static_cast<int>(res));
        return false;
    }
}

bool mtl_commit_command_buffer_internal(void *cb_obj, bool wait_completed) {
    MtlVkState &st = get_state();
    auto *cb = unwrap_mtl<MtlCommandBufferImpl>(cb_obj);
    if (!st.initialized || !cb) return false;
    if (cb->committed) {
        if (wait_completed && cb->status < 4) {
            wait_slot_idle(st, cb->frame_idx);
        }
        return true;
    }

    if (!cb->recording || cb->cmd == VK_NULL_HANDLE) {
        /* Empty command buffer without any encoder calls: open/close so fence + present remain valid */
        if (mtl_begin_command_buffer_internal(cb) == VK_NULL_HANDLE) return false;
    }

    std::unique_lock<std::mutex> surface_lock(st.surface_mutex, std::defer_lock);
    bool do_present = false;
    if (cb->present_requested) {
        surface_lock.lock();
        do_present = (!st.surface_paused && !st.surface_shutting_down &&
                      cb->present_surface_gen == st.surface_gen &&
                      st.vk_ctx.swapchain() != VK_NULL_HANDLE &&
                      cb->present_sc_idx < st.vk_ctx.swapchain_images().size() &&
                      cb->present_sc_idx < st.render_finished_sems.size());
    }

    if (do_present && cb->present_tex_obj) {
        auto *tex = unwrap_mtl<MtlTextureImpl>(cb->present_tex_obj);
        if (tex && tex->is_swapchain) {
            VkImageLayout cur_layout = tex->mip_layouts.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : tex->mip_layouts[0];
            if (cur_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                transition_image_layout(cb->cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        0, 1, 0, 1, cur_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                cur_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            if (st.use_virtual_drawable && cb->present_sc_idx < st.vk_ctx.swapchain_images().size()) {
                VkImage sc_img = st.vk_ctx.swapchain_images()[cb->present_sc_idx];
                VkExtent2D sc_ext = st.vk_ctx.swapchain_extent();
                transition_image_layout(cb->cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        0, 1, 0, 1, cur_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                transition_image_layout(cb->cmd, sc_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        0, 1, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[0] = {0, 0, 0};
                blit.srcOffsets[1] = {(int32_t)tex->width, (int32_t)tex->height, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[0] = {0, 0, 0};
                blit.dstOffsets[1] = {(int32_t)sc_ext.width, (int32_t)sc_ext.height, 1};
                vkCmdBlitImage(cb->cmd,
                               tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               sc_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
                transition_image_layout(cb->cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                        0, 1, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                transition_image_layout(cb->cmd, sc_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        0, 1, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                if (!tex->mip_layouts.empty()) tex->mip_layouts[0] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                if (cb->present_sc_idx < st.swapchain_layouts.size()) {
                    st.swapchain_layouts[cb->present_sc_idx] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                }
            } else {
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
    }

    FrameSyncSlot &slot = st.frames[cb->frame_idx];
    if (slot.ts_capturing && slot.ts_query_pool != VK_NULL_HANDLE && slot.ts_query_cursor < kMaxTimestampQueriesPerCB) {
        vkCmdWriteTimestamp(cb->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.ts_query_pool, slot.ts_query_cursor++);
    }
    if (vkEndCommandBuffer(cb->cmd) != VK_SUCCESS) {
        MTLVK_LOGE("vkEndCommandBuffer failed");
        return false;
    }
    cb->recording = false;
    cb->committed = true;
    cb->status = 2; /* Committed */

    const bool has_preamble = mtl_record_preamble_btex_copies(cb);
    if (slot.inline_cursor > 0 && slot.inline_alloc != VK_NULL_HANDLE) {
        vmaFlushAllocation(st.vk_ctx.allocator(), slot.inline_alloc, 0, slot.inline_cursor);
    }
    VkSemaphore wait_sems[1] = {cb->acquire_wait_sem};
    VkPipelineStageFlags wait_stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signal_sems[1] = {};
    uint32_t signal_count = 0;
    uint32_t wait_count = (do_present && cb->acquire_wait_sem != VK_NULL_HANDLE) ? 1u : 0u;

    if (do_present && cb->present_sc_idx < st.render_finished_sems.size()) {
        signal_sems[0] = st.render_finished_sems[cb->present_sc_idx];
        signal_count = 1;
    }

    VkCommandBuffer submit_cmds[2] = {has_preamble ? slot.preamble_cmd : cb->cmd, cb->cmd};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = wait_count;
    submit.pWaitSemaphores = wait_count ? wait_sems : nullptr;
    submit.pWaitDstStageMask = wait_count ? wait_stages : nullptr;
    submit.commandBufferCount = has_preamble ? 2u : 1u;
    submit.pCommandBuffers = submit_cmds;
    submit.signalSemaphoreCount = signal_count;
    submit.pSignalSemaphores = signal_count ? signal_sems : nullptr;

    {
        std::lock_guard<std::mutex> clk(st.completion_mutex);
        st.slot_in_flight[cb->frame_idx] = true;
        slot.submitted = true;
    }

    uint64_t submit_now_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
    if (cb->begin_ns > 0 && submit_now_ns > cb->begin_ns) {
        uint64_t cb_rec_ns = submit_now_ns - cb->begin_ns;
        st.prof_cb_rec_us.fetch_add(cb_rec_ns / 1000ULL, std::memory_order_relaxed);
        g_adpf.frame_accum_work_ns += cb_rec_ns;
    }
    VkResult qres = VK_SUCCESS;
    {
        auto t_sub0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> qlk(st.queue_mutex);
        qres = vkQueueSubmit(st.vk_ctx.graphics_queue(), 1, &submit, slot.in_flight_fence);
        auto sub_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_sub0).count();
        st.prof_sub_us.fetch_add((uint64_t)sub_us, std::memory_order_relaxed);
    }
    if (qres == VK_SUCCESS && slot.staging_end_cursor > 0) {
        st.staging.track_submission(slot.staging_end_cursor, slot.in_flight_fence);
        slot.staging_end_cursor = 0;
    }
    if (qres != VK_SUCCESS) {
        {
            std::lock_guard<std::mutex> clk(st.completion_mutex);
            st.slot_in_flight[cb->frame_idx] = false;
            slot.submitted = false;
        }
        st.completion_done_cv.notify_all();
        MTLVK_LOGE("vkQueueSubmit failed: %d", static_cast<int>(qres));
        return false;
    }
    st.command_buffers_committed.fetch_add(1, std::memory_order_relaxed);

    {
        std::vector<void *> sched;
        sched.swap(cb->scheduled_blocks);
        for (void *blk : sched) {
            invoke_objc_block_1(blk, cb_obj);
            release_objc_block(blk);
        }
    }

    if (do_present && signal_count == 1) {
        const int target_fps = oh2_cadisplaylink_get_target_fps();
        g_adpf.sync_target_fps(st.current_window, target_fps);
        if (g_adpf.frame_accum_work_ns > 0) {
            g_adpf.report_work_ns(static_cast<int64_t>(g_adpf.frame_accum_work_ns));
            g_adpf.frame_accum_work_ns = 0;
        }
        st.present_ring_fidx[st.present_ring_pos] = cb->frame_idx;
        st.present_ring_pos = (st.present_ring_pos + 1u) & 1u;
        VkSwapchainKHR sc = st.vk_ctx.swapchain();
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &signal_sems[0];
        present.swapchainCount = 1;
        present.pSwapchains = &sc;
        present.pImageIndices = &cb->present_sc_idx;
        VkResult pres = VK_SUCCESS;
        {
            auto t_pr0 = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> qlk(st.queue_mutex);
            pres = vkQueuePresentKHR(st.vk_ctx.graphics_queue(), &present);
            auto pr_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_pr0).count();
            st.prof_pres_us.fetch_add((uint64_t)pr_us, std::memory_order_relaxed);
        }
        if (pres == VK_ERROR_OUT_OF_DATE_KHR) {
            st.swapchain_out_of_date = true;
        } else if (pres == VK_SUBOPTIMAL_KHR || pres == VK_SUCCESS) {
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
                static auto s_prev_tp = std::chrono::steady_clock::now();
                static uint64_t s_prev_draws = 0, s_prev_idraws = 0, s_prev_fb = 0, s_prev_occ = 0, s_prev_bt = 0, s_prev_rp = 0, s_prev_wait = 0, s_prev_flush = 0, s_prev_acq = 0, s_prev_sub = 0, s_prev_pres = 0, s_prev_iu_us = 0, s_prev_iu_cnt = 0, s_prev_dh = 0, s_prev_dm = 0;
                auto now_tp = std::chrono::steady_clock::now();
                double dt_ms = std::chrono::duration<double, std::milli>(now_tp - s_prev_tp).count();
                s_prev_tp = now_tp;
                uint64_t d_dr = st.draw_calls.load(std::memory_order_relaxed) - s_prev_draws;
                uint64_t d_id = st.indexed_draw_calls.load(std::memory_order_relaxed) - s_prev_idraws;
                uint64_t d_fb = st.prof_fb_snaps.load(std::memory_order_relaxed) - s_prev_fb;
                uint64_t d_oc = st.prof_occ_queries.load(std::memory_order_relaxed) - s_prev_occ;
                uint64_t d_bt = st.prof_buftex_uploads.load(std::memory_order_relaxed) - s_prev_bt;
                uint64_t d_rp = st.prof_render_passes.load(std::memory_order_relaxed) - s_prev_rp;
                uint64_t d_wt = st.prof_wait_slot_us.load(std::memory_order_relaxed) - s_prev_wait;
                uint64_t d_fl = st.prof_flush_us.load(std::memory_order_relaxed) - s_prev_flush;
                uint64_t d_aq = st.prof_acq_us.load(std::memory_order_relaxed) - s_prev_acq;
                uint64_t d_sb = st.prof_sub_us.load(std::memory_order_relaxed) - s_prev_sub;
                uint64_t d_pr = st.prof_pres_us.load(std::memory_order_relaxed) - s_prev_pres;
                uint64_t d_iu = st.prof_imm_up_us.load(std::memory_order_relaxed) - s_prev_iu_us;
                uint64_t d_ic = st.prof_imm_up_cnt.load(std::memory_order_relaxed) - s_prev_iu_cnt;
                uint64_t d_dh = st.prof_desc_hits.load(std::memory_order_relaxed) - s_prev_dh;
                uint64_t d_dm = st.prof_desc_misses.load(std::memory_order_relaxed) - s_prev_dm;
                s_prev_draws += d_dr; s_prev_idraws += d_id; s_prev_fb += d_fb; s_prev_occ += d_oc;
                s_prev_bt += d_bt; s_prev_rp += d_rp; s_prev_wait += d_wt; s_prev_flush += d_fl;
                s_prev_acq += d_aq; s_prev_sub += d_sb; s_prev_pres += d_pr;
                s_prev_iu_us += d_iu; s_prev_iu_cnt += d_ic;
                s_prev_dh += d_dh; s_prev_dm += d_dm;
                if (pcount >= 60 && dt_ms > 1.0) {
                    static uint64_t s_prev_gf = 0, s_prev_rec = 0, s_prev_drst = 0;
                    uint64_t d_gf = st.prof_gpu_fence_us.load(std::memory_order_relaxed) - s_prev_gf;
                    uint64_t d_rec = st.prof_cb_rec_us.load(std::memory_order_relaxed) - s_prev_rec;
                    uint64_t d_drst = st.prof_desc_resets.load(std::memory_order_relaxed) - s_prev_drst;
                    s_prev_gf += d_gf;
                    s_prev_rec += d_rec;
                    s_prev_drst += d_drst;
                    MTLVK_LOGI("[M5] Prof60: fps=%.1f frame=%.1fms rec=%.1fms draws/f=%.1f rp/f=%.1f fb_snap/f=%.1f occ/f=%.1f btex/f=%.1f dcache=%.0f/%.0f drst=%llu wait=%.1f",
                               60000.0 / dt_ms, dt_ms / 60.0, (double)d_rec / 60000.0,
                               (double)(d_dr + d_id) / 60.0,
                               (double)d_rp / 60.0, (double)d_fb / 60.0, (double)d_oc / 60.0,
                               (double)d_bt / 60.0, (double)d_dh / 60.0, (double)(d_dh + d_dm) / 60.0,
                               (unsigned long long)d_drst,
                               (double)d_wt / 60000.0);
                }
            }
        } else {
            MTLVK_LOGE("vkQueuePresentKHR failed: %d", static_cast<int>(pres));
        }
        if (st.shared_layer_obj) {
            auto *layer = unwrap_mtl<MtlMetalLayerImpl>(st.shared_layer_obj);
            if (layer) layer->has_active_drawable = false;
        }
    } else if (cb->present_requested && st.shared_layer_obj) {
        auto *layer = unwrap_mtl<MtlMetalLayerImpl>(st.shared_layer_obj);
        if (layer) layer->has_active_drawable = false;
    }
    if (surface_lock.owns_lock()) {
        surface_lock.unlock();
    }

    {
        std::lock_guard<std::mutex> clk(st.completion_mutex);
        PendingCbCompletion pcc{};
        pcc.cb_obj = cb_obj;
        pcc.begin_ns = cb->begin_ns;
        pcc.submit_ns = submit_now_ns;
        pcc.present_frame_tag = st.presents.load(std::memory_order_relaxed);
        oh2_objc_add_refcount(cb_obj, 1);
        pcc.blocks.swap(cb->completed_blocks);
        st.completion_queue.emplace_back(cb->frame_idx, std::move(pcc));
    }
    st.completion_cv.notify_one();

    if (wait_completed) {
        wait_slot_idle(st, cb->frame_idx);
        tick_deferred_destroys(false);
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
    for (VkImageView v : st.virtual_drawable_views) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
    }
    for (size_t i = 0; i < st.virtual_drawable_images.size(); ++i) {
        if (st.virtual_drawable_images[i] != VK_NULL_HANDLE) {
            vmaDestroyImage(st.vk_ctx.allocator(), st.virtual_drawable_images[i], st.virtual_drawable_allocs[i]);
        }
    }
    st.virtual_drawable_views.clear();
    st.virtual_drawable_images.clear();
    st.virtual_drawable_allocs.clear();
    st.swapchain_layouts.clear();

    const auto &imgs = st.vk_ctx.swapchain_images();
    const auto &views = st.vk_ctx.swapchain_image_views();
    const VkExtent2D ext = st.vk_ctx.swapchain_extent();
    const VkFormat sc_fmt = st.vk_ctx.swapchain_format();
    const uint64_t mtl_fmt = (sc_fmt == VK_FORMAT_B8G8R8A8_UNORM) ? 80 :
                             (sc_fmt == VK_FORMAT_B8G8R8A8_SRGB) ? 81 :
                             (sc_fmt == VK_FORMAT_R8G8B8A8_SRGB) ? 71 : 70;

    /* Scale high-DPI mobile screens (>720p height) to crisp iPhone-class 720p virtual drawable
       with exact aspect ratio, then hardware-blit linearly to physical swapchain for 2.25x GPU speedup. */
    uint32_t virt_w = ext.width;
    uint32_t virt_h = ext.height;
    if (ext.height > 720u && ext.width > ext.height) {
        virt_h = 720u;
        virt_w = ((ext.width * virt_h) / ext.height + 3u) & ~3u;
    }
    st.virtual_render_w = virt_w;
    st.virtual_render_h = virt_h;
    st.use_virtual_drawable = (virt_w != ext.width || virt_h != ext.height);

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

    if (st.use_virtual_drawable) {
        st.virtual_drawable_images.resize(n, VK_NULL_HANDLE);
        st.virtual_drawable_allocs.resize(n, VK_NULL_HANDLE);
        st.virtual_drawable_views.resize(n, VK_NULL_HANDLE);
        for (size_t i = 0; i < n; ++i) {
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = sc_fmt;
            ici.extent = {virt_w, virt_h, 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo vai{};
            vai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(st.vk_ctx.allocator(), &ici, &vai,
                               &st.virtual_drawable_images[i], &st.virtual_drawable_allocs[i], nullptr) != VK_SUCCESS) {
                st.use_virtual_drawable = false;
                st.virtual_render_w = ext.width;
                st.virtual_render_h = ext.height;
                break;
            }
            VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ivci.image = st.virtual_drawable_images[i];
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format = sc_fmt;
            ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(dev, &ivci, nullptr, &st.virtual_drawable_views[i]) != VK_SUCCESS) {
                st.use_virtual_drawable = false;
                st.virtual_render_w = ext.width;
                st.virtual_render_h = ext.height;
                break;
            }
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
        tex->image = st.use_virtual_drawable ? st.virtual_drawable_images[i] : imgs[i];
        tex->unique_id = next_mtl_resource_uid();
        tex->view = st.use_virtual_drawable ? st.virtual_drawable_views[i] : ((i < views.size()) ? views[i] : VK_NULL_HANDLE);
        tex->vk_format = sc_fmt;
        tex->mtl_pixel_format = mtl_fmt;
        tex->texture_type = kMTLTextureType2D;
        tex->width = st.virtual_render_w;
        tex->height = st.virtual_render_h;
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
    cb->present_surface_gen = dr->surface_gen;
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
    std::vector<uint8_t> disk_pcache;
    {
        const char *broot = oh2_objc_get_bundle_root();
        if (broot && broot[0]) {
            std::string ppath = std::string(broot) + "/vk_pipeline_cache.bin";
            FILE *fp = std::fopen(ppath.c_str(), "rb");
            if (fp) {
                std::fseek(fp, 0, SEEK_END);
                long sz = std::ftell(fp);
                std::fseek(fp, 0, SEEK_SET);
                if (sz > 16 && sz < 64 * 1024 * 1024) {
                    disk_pcache.resize((size_t)sz);
                    if (std::fread(disk_pcache.data(), 1, (size_t)sz, fp) != (size_t)sz) {
                        disk_pcache.clear();
                    }
                }
                std::fclose(fp);
            }
        }
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (!disk_pcache.empty()) {
        pci.initialDataSize = disk_pcache.size();
        pci.pInitialData = disk_pcache.data();
    }
    if (vkCreatePipelineCache(dev, &pci, nullptr, &st.pipeline_cache) != VK_SUCCESS && !disk_pcache.empty()) {
        pci.initialDataSize = 0;
        pci.pInitialData = nullptr;
        vkCreatePipelineCache(dev, &pci, nullptr, &st.pipeline_cache);
    } else if (!disk_pcache.empty()) {
        MTLVK_LOGI("Loaded persistent VkPipelineCache (%zu bytes)", disk_pcache.size());
    }

    VkCommandPoolCreateInfo up_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    up_pool.queueFamilyIndex = st.vk_ctx.graphics_queue_family();
    up_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(dev, &up_pool, nullptr, &st.upload_pool) != VK_SUCCESS) return -1;
    VkFenceCreateInfo uf_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev, &uf_info, nullptr, &st.upload_fence) != VK_SUCCESS) return -1;

    VkPhysicalDeviceProperties pdp{};
    vkGetPhysicalDeviceProperties(st.vk_ctx.physical_device(), &pdp);
    st.timestamp_period_ns = (pdp.limits.timestampPeriod > 0.0f) ? pdp.limits.timestampPeriod : 1.0f;
    st.max_dyn_ubo_per_stage = std::min<uint32_t>(8u, pdp.limits.maxDescriptorSetUniformBuffersDynamic / 2u);
    st.min_ubo_offset_align = std::max<uint64_t>(1ULL, pdp.limits.minUniformBufferOffsetAlignment);
    MTLVK_LOGI("[M5] GPU limits: maxDynUBO=%u (perStage=%u) maxDynSSBO=%u minUboAlign=%llu tsPeriod=%.2fns",
               pdp.limits.maxDescriptorSetUniformBuffersDynamic,
               st.max_dyn_ubo_per_stage,
               pdp.limits.maxDescriptorSetStorageBuffersDynamic,
               (unsigned long long)st.min_ubo_offset_align,
               st.timestamp_period_ns);

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        FrameSyncSlot &f = st.frames[i];
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = st.vk_ctx.graphics_queue_family();
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(dev, &cpi, nullptr, &f.cmd_pool) != VK_SUCCESS) return -1;

        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = f.cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 2;
        VkCommandBuffer cmds[2]{};
        if (vkAllocateCommandBuffers(dev, &cbai, cmds) != VK_SUCCESS) return -1;
        f.cmd = cmds[0];
        f.preamble_cmd = cmds[1];

        f.desc_pool = create_frame_descriptor_pool(dev);
        if (f.desc_pool == VK_NULL_HANDLE) return -1;
        f.overflow_desc_pools.clear();
        f.active_desc_pool_idx = 0;

        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_OCCLUSION;
        qpci.queryCount = kMaxOcclusionQueriesPerCB;
        if (vkCreateQueryPool(dev, &qpci, nullptr, &f.occ_query_pool) != VK_SUCCESS) {
            f.occ_query_pool = VK_NULL_HANDLE;
        }
        f.occ_query_cursor = 0;

        VkQueryPoolCreateInfo tqpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        tqpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        tqpci.queryCount = kMaxTimestampQueriesPerCB;
        if (vkCreateQueryPool(dev, &tqpci, nullptr, &f.ts_query_pool) != VK_SUCCESS) {
            f.ts_query_pool = VK_NULL_HANDLE;
        }
        f.ts_query_cursor = 0;
        f.ts_capturing = false;

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(dev, &si, nullptr, &f.image_available_sem) != VK_SUCCESS) return -1;

        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fi, nullptr, &f.in_flight_fence) != VK_SUCCESS) return -1;
        f.submitted = false;

        f.inline_capacity = 8ULL * 1024ULL * 1024ULL;
        VkBufferCreateInfo ibci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ibci.size = f.inline_capacity;
        ibci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
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
    st.current_window = window;
    st.surface_paused = false;
    st.surface_shutting_down = false;
    st.initialized = true;
    if (!create_dummy_resources(st)) {
        st.initialized = false;
        return -1;
    }

    {
        std::lock_guard<std::mutex> clk(st.completion_mutex);
        st.completion_stop = false;
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) st.slot_in_flight[i] = false;
        st.completion_queue.clear();
        st.completion_thread = std::thread(completion_thread_fn, &st);
        st.completion_thread_started = true;
    }

    if (spirv_cache_path && spirv_cache_path[0]) {
        bool cache_ok = spirv_cache_load(spirv_cache_path);
        MTLVK_LOGI("SPIR-V cache load('%s'): ok=%d entries=%u",
                   spirv_cache_path, cache_ok ? 1 : 0, spirv_cache_count());
    }

    VkExtent2D ext = st.vk_ctx.swapchain_extent();
    oh2_ui_set_physical_dimensions(ext.width, ext.height);
    oh2_ui_set_screen_dimensions(st.virtual_render_w, st.virtual_render_h, 2.0);
    MTLVK_LOGI("[M5] mtl_vk_init ready: GPU=%s swapchain=%ux%u render=%ux%u dyn_render=%d astc=(%d,%d,%d) val=%d",
               st.vk_ctx.caps().device_name, ext.width, ext.height,
               st.virtual_render_w, st.virtual_render_h,
               st.vk_ctx.caps().dynamic_rendering ? 1 : 0,
               st.vk_ctx.caps().astc_4x4 ? 1 : 0,
               st.vk_ctx.caps().astc_6x6 ? 1 : 0,
               st.vk_ctx.caps().astc_8x8 ? 1 : 0,
               st.vk_ctx.caps().validation_layer_enabled ? 1 : 0);
    return 0;
}

void mtl_vk_pause_surface(void) {
    using namespace oh2::mtlvk;
    MtlVkState &st = get_state();
    if (!st.initialized) return;
    std::lock_guard<std::mutex> slk(st.surface_mutex);
    if (st.surface_paused) return;
    st.surface_paused = true;
    VkDevice dev = st.vk_ctx.device();
    wait_all_slots_idle(st);
    {
        std::lock_guard<std::mutex> qlk(st.queue_mutex);
        vkDeviceWaitIdle(dev);
    }
    tick_deferred_destroys(true);
    for (int bi = 0; bi < 5; ++bi) {
        for (const auto &pb : st.buffer_pool[bi]) {
            if (pb.buffer != VK_NULL_HANDLE || pb.allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(st.vk_ctx.allocator(), pb.buffer, pb.allocation);
            }
        }
        st.buffer_pool[bi].clear();
    }
    st.vk_ctx.release_surface();
    st.current_window = nullptr;
    if (st.shared_layer_obj) {
        auto *l = unwrap_mtl<MtlMetalLayerImpl>(st.shared_layer_obj);
        if (l) l->has_active_drawable = false;
    }
    MTLVK_LOGI("[M5] Swapchain surface paused & released (background)");
}

void mtl_vk_begin_shutdown(void) {
    using namespace oh2::mtlvk;
    MtlVkState &st = get_state();
    {
        std::lock_guard<std::mutex> slk(st.surface_mutex);
        st.surface_shutting_down = true;
    }
    st.surface_cv.notify_all();
}

int mtl_vk_set_surface(ANativeWindow *window) {
    using namespace oh2::mtlvk;
    MtlVkState &st = get_state();
    if (!st.initialized || !window) return -1;
    {
        std::lock_guard<std::mutex> slk(st.surface_mutex);
        VkExtent2D cur_ext = st.vk_ctx.swapchain_extent();
        int32_t nw = ANativeWindow_getWidth(window);
        int32_t nh = ANativeWindow_getHeight(window);
        if (window == st.current_window && !st.surface_paused && !st.swapchain_out_of_date &&
            st.vk_ctx.swapchain() != VK_NULL_HANDLE &&
            nw > 0 && nh > 0 &&
            static_cast<uint32_t>(nw) == cur_ext.width &&
            static_cast<uint32_t>(nh) == cur_ext.height) {
            return 0;
        }
        VkDevice dev = st.vk_ctx.device();
        wait_all_slots_idle(st);
        {
            std::lock_guard<std::mutex> qlk(st.queue_mutex);
            vkDeviceWaitIdle(dev);
        }
        tick_deferred_destroys(true);
    for (int bi = 0; bi < 5; ++bi) {
        for (const auto &pb : st.buffer_pool[bi]) {
            if (pb.buffer != VK_NULL_HANDLE || pb.allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(st.vk_ctx.allocator(), pb.buffer, pb.allocation);
            }
        }
        st.buffer_pool[bi].clear();
    }
        if (!st.vk_ctx.rebind_surface(window)) {
            MTLVK_LOGE("VkContext::rebind_surface failed");
            return -1;
        }
        if (!create_per_swapchain_sync(st)) return -1;
        st.current_window = window;
        st.swapchain_out_of_date = false;
        st.surface_paused = false;
        st.surface_gen++;
        st.surface_recreates.fetch_add(1, std::memory_order_relaxed);
        VkExtent2D ext = st.vk_ctx.swapchain_extent();
        oh2_ui_set_physical_dimensions(ext.width, ext.height);
        oh2_ui_set_screen_dimensions(st.virtual_render_w, st.virtual_render_h, 2.0);
        if (st.shared_layer_obj) {
            auto *l = unwrap_mtl<MtlMetalLayerImpl>(st.shared_layer_obj);
            if (l) {
                l->drawable_width = (double)st.virtual_render_w;
                l->drawable_height = (double)st.virtual_render_h;
                l->has_active_drawable = false;
            }
        }
        MTLVK_LOGI("[M5] Swapchain surface rebuilt #%llu: %ux%u (resumed)",
                   (unsigned long long)st.surface_recreates.load(), ext.width, ext.height);
    }
    st.surface_cv.notify_all();
    return 0;
}

void mtl_vk_shutdown(void) {
    using namespace oh2::mtlvk;
    MtlVkState &st = get_state();
    if (!st.initialized) return;
    wait_all_slots_idle(st);
    if (st.completion_thread_started) {
        {
            std::lock_guard<std::mutex> clk(st.completion_mutex);
            st.completion_stop = true;
        }
        st.completion_cv.notify_all();
        if (st.completion_thread.joinable()) {
            st.completion_thread.join();
        }
        st.completion_thread_started = false;
    }
    VkDevice dev = st.vk_ctx.device();
    {
        std::lock_guard<std::mutex> qlk(st.queue_mutex);
        vkDeviceWaitIdle(dev);
    }
    tick_deferred_destroys(true);
    for (int bi = 0; bi < 5; ++bi) {
        for (const auto &pb : st.buffer_pool[bi]) {
            if (pb.buffer != VK_NULL_HANDLE || pb.allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(st.vk_ctx.allocator(), pb.buffer, pb.allocation);
            }
        }
        st.buffer_pool[bi].clear();
    }

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
    for (auto &fe : st.fb_fetch_pool) {
        if (fe.view != VK_NULL_HANDLE) vkDestroyImageView(dev, fe.view, nullptr);
        if (fe.image != VK_NULL_HANDLE) vmaDestroyImage(st.vk_ctx.allocator(), fe.image, fe.alloc);
    }
    st.fb_fetch_pool.clear();

    for (VkSemaphore s : st.render_finished_sems) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(dev, s, nullptr);
    }
    st.render_finished_sems.clear();
    for (VkImageView v : st.virtual_drawable_views) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
    }
    for (size_t i = 0; i < st.virtual_drawable_images.size(); ++i) {
        if (st.virtual_drawable_images[i] != VK_NULL_HANDLE) {
            vmaDestroyImage(st.vk_ctx.allocator(), st.virtual_drawable_images[i], st.virtual_drawable_allocs[i]);
        }
    }
    st.virtual_drawable_views.clear();
    st.virtual_drawable_images.clear();
    st.virtual_drawable_allocs.clear();

    for (uint32_t i = 0; i < 8; ++i) {
        if (st.acquire_sem_pool[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, st.acquire_sem_pool[i], nullptr);
            st.acquire_sem_pool[i] = VK_NULL_HANDLE;
        }
    }

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (st.frames[i].occ_query_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(dev, st.frames[i].occ_query_pool, nullptr);
            st.frames[i].occ_query_pool = VK_NULL_HANDLE;
        }
        if (st.frames[i].ts_query_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(dev, st.frames[i].ts_query_pool, nullptr);
            st.frames[i].ts_query_pool = VK_NULL_HANDLE;
        }
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
    if (st.pipeline_cache != VK_NULL_HANDLE) {
        const char *broot = oh2_objc_get_bundle_root();
        if (broot && broot[0]) {
            size_t pc_sz = 0;
            if (vkGetPipelineCacheData(dev, st.pipeline_cache, &pc_sz, nullptr) == VK_SUCCESS && pc_sz > 16) {
                std::vector<uint8_t> pc_data(pc_sz);
                if (vkGetPipelineCacheData(dev, st.pipeline_cache, &pc_sz, pc_data.data()) == VK_SUCCESS && pc_sz > 16) {
                    std::string ppath = std::string(broot) + "/vk_pipeline_cache.bin";
                    std::string tpath = ppath + ".tmp";
                    FILE *fp = std::fopen(tpath.c_str(), "wb");
                    if (fp) {
                        if (std::fwrite(pc_data.data(), 1, pc_sz, fp) == pc_sz) {
                            std::fclose(fp);
                            std::remove(ppath.c_str());
                            std::rename(tpath.c_str(), ppath.c_str());
                            MTLVK_LOGI("Saved persistent VkPipelineCache (%zu bytes)", pc_sz);
                        } else {
                            std::fclose(fp);
                            std::remove(tpath.c_str());
                        }
                    }
                }
            }
        }
        vkDestroyPipelineCache(dev, st.pipeline_cache, nullptr);
    }

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
