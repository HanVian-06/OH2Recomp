#include "mtl_vk_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace oh2::mtlvk {

VkFormat mtl_pixel_format_to_vk(uint64_t mtl_fmt, bool *out_is_astc, bool *out_is_depth, bool *out_has_stencil) {
    bool astc = false;
    bool depth = false;
    bool stencil = false;
    VkFormat vk = VK_FORMAT_B8G8R8A8_UNORM;

    switch (mtl_fmt) {
        case 0:   vk = VK_FORMAT_UNDEFINED; break;
        case 1:   vk = VK_FORMAT_A8_UNORM_KHR; break; /* A8Unorm? fallback R8 below if needed */
        case 10:  vk = VK_FORMAT_R8_UNORM; break;
        case 11:  vk = VK_FORMAT_R8_SRGB; break;
        case 12:  vk = VK_FORMAT_R8_SNORM; break;
        case 13:  vk = VK_FORMAT_R8_UINT; break;
        case 14:  vk = VK_FORMAT_R8_SINT; break;
        case 20:  vk = VK_FORMAT_R16_UNORM; break;
        case 22:  vk = VK_FORMAT_R16_SNORM; break;
        case 23:  vk = VK_FORMAT_R16_UINT; break;
        case 24:  vk = VK_FORMAT_R16_SINT; break;
        case 25:  vk = VK_FORMAT_R16_SFLOAT; break;
        case 30:  vk = VK_FORMAT_R8G8_UNORM; break;
        case 31:  vk = VK_FORMAT_R8G8_SRGB; break;
        case 32:  vk = VK_FORMAT_R8G8_SNORM; break;
        case 33:  vk = VK_FORMAT_R8G8_UINT; break;
        case 34:  vk = VK_FORMAT_R8G8_SINT; break;
        case 40:  vk = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
        case 41:  vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
        case 42:  vk = VK_FORMAT_R4G4B4A4_UNORM_PACK16; break;
        case 43:  vk = VK_FORMAT_R5G5B5A1_UNORM_PACK16; break;
        case 53:  vk = VK_FORMAT_R32_UINT; break;
        case 54:  vk = VK_FORMAT_R32_SINT; break;
        case 55:  vk = VK_FORMAT_R32_SFLOAT; break;
        case 60:  vk = VK_FORMAT_R16G16_UNORM; break;
        case 62:  vk = VK_FORMAT_R16G16_SNORM; break;
        case 63:  vk = VK_FORMAT_R16G16_UINT; break;
        case 64:  vk = VK_FORMAT_R16G16_SINT; break;
        case 65:  vk = VK_FORMAT_R16G16_SFLOAT; break;
        case 70:  vk = VK_FORMAT_R8G8B8A8_UNORM; break;
        case 71:  vk = VK_FORMAT_R8G8B8A8_SRGB; break;
        case 72:  vk = VK_FORMAT_R8G8B8A8_SNORM; break;
        case 73:  vk = VK_FORMAT_R8G8B8A8_UINT; break;
        case 74:  vk = VK_FORMAT_R8G8B8A8_SINT; break;
        case 80:  vk = VK_FORMAT_B8G8R8A8_UNORM; break;
        case 81:  vk = VK_FORMAT_B8G8R8A8_SRGB; break;
        case 90:  vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
        case 91:  vk = VK_FORMAT_A2B10G10R10_UINT_PACK32; break;
        case 92:  vk = VK_FORMAT_B10G11R11_UFLOAT_PACK32; break;
        case 93:  vk = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32; break;
        case 94:  vk = VK_FORMAT_A2R10G10B10_UNORM_PACK32; break;
        case 103: vk = VK_FORMAT_R32G32_UINT; break;
        case 104: vk = VK_FORMAT_R32G32_SINT; break;
        case 105: vk = VK_FORMAT_R32G32_SFLOAT; break;
        case 110: vk = VK_FORMAT_R16G16B16A16_UNORM; break;
        case 112: vk = VK_FORMAT_R16G16B16A16_SNORM; break;
        case 113: vk = VK_FORMAT_R16G16B16A16_UINT; break;
        case 114: vk = VK_FORMAT_R16G16B16A16_SINT; break;
        case 115: vk = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        case 123: vk = VK_FORMAT_R32G32B32A32_UINT; break;
        case 124: vk = VK_FORMAT_R32G32B32A32_SINT; break;
        case 125: vk = VK_FORMAT_R32G32B32A32_SFLOAT; break;

        /* Native iOS ASTC formats mapped directly to exact Apple MTLPixelFormat enum values */
        case 186: vk = VK_FORMAT_ASTC_4x4_SRGB_BLOCK;   astc = true; break;
        case 187: vk = VK_FORMAT_ASTC_5x4_SRGB_BLOCK;   astc = true; break;
        case 188: vk = VK_FORMAT_ASTC_5x5_SRGB_BLOCK;   astc = true; break;
        case 189: vk = VK_FORMAT_ASTC_6x5_SRGB_BLOCK;   astc = true; break;
        case 190: vk = VK_FORMAT_ASTC_6x6_SRGB_BLOCK;   astc = true; break;
        case 192: vk = VK_FORMAT_ASTC_8x5_SRGB_BLOCK;   astc = true; break;
        case 193: vk = VK_FORMAT_ASTC_8x6_SRGB_BLOCK;   astc = true; break;
        case 194: vk = VK_FORMAT_ASTC_8x8_SRGB_BLOCK;   astc = true; break;
        case 195: vk = VK_FORMAT_ASTC_10x5_SRGB_BLOCK;  astc = true; break;
        case 196: vk = VK_FORMAT_ASTC_10x6_SRGB_BLOCK;  astc = true; break;
        case 197: vk = VK_FORMAT_ASTC_10x8_SRGB_BLOCK;  astc = true; break;
        case 198: vk = VK_FORMAT_ASTC_10x10_SRGB_BLOCK; astc = true; break;
        case 199: vk = VK_FORMAT_ASTC_12x10_SRGB_BLOCK; astc = true; break;
        case 200: vk = VK_FORMAT_ASTC_12x12_SRGB_BLOCK; astc = true; break;

        case 204: vk = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;   astc = true; break;
        case 205: vk = VK_FORMAT_ASTC_5x4_UNORM_BLOCK;   astc = true; break;
        case 206: vk = VK_FORMAT_ASTC_5x5_UNORM_BLOCK;   astc = true; break;
        case 207: vk = VK_FORMAT_ASTC_6x5_UNORM_BLOCK;   astc = true; break;
        case 208: vk = VK_FORMAT_ASTC_6x6_UNORM_BLOCK;   astc = true; break;
        case 210: vk = VK_FORMAT_ASTC_8x5_UNORM_BLOCK;   astc = true; break;
        case 211: vk = VK_FORMAT_ASTC_8x6_UNORM_BLOCK;   astc = true; break;
        case 212: vk = VK_FORMAT_ASTC_8x8_UNORM_BLOCK;   astc = true; break;
        case 213: vk = VK_FORMAT_ASTC_10x5_UNORM_BLOCK;  astc = true; break;
        case 214: vk = VK_FORMAT_ASTC_10x6_UNORM_BLOCK;  astc = true; break;
        case 215: vk = VK_FORMAT_ASTC_10x8_UNORM_BLOCK;  astc = true; break;
        case 216: vk = VK_FORMAT_ASTC_10x10_UNORM_BLOCK; astc = true; break;
        case 217: vk = VK_FORMAT_ASTC_12x10_UNORM_BLOCK; astc = true; break;
        case 218: vk = VK_FORMAT_ASTC_12x12_UNORM_BLOCK; astc = true; break;

        /* Depth / Stencil */
        case 250: vk = VK_FORMAT_D16_UNORM; depth = true; break;
        case 252: vk = VK_FORMAT_D32_SFLOAT; depth = true; break;
        case 253: vk = VK_FORMAT_S8_UINT; stencil = true; break;
        case 255: vk = VK_FORMAT_D32_SFLOAT_S8_UINT; depth = true; stencil = true; break;
        case 260: vk = VK_FORMAT_D32_SFLOAT_S8_UINT; depth = true; stencil = true; break;
        default:  vk = VK_FORMAT_R8G8B8A8_UNORM; break;
    }

    if (mtl_fmt == 1) vk = VK_FORMAT_R8_UNORM;
    if (out_is_astc) *out_is_astc = astc;
    if (out_is_depth) *out_is_depth = depth;
    if (out_has_stencil) *out_has_stencil = stencil;
    return vk;
}

uint32_t mtl_pixel_format_block_size(VkFormat fmt, uint32_t *out_bw, uint32_t *out_bh) {
    uint32_t bw = 1, bh = 1, bsz = 4;
    switch (fmt) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_S8_UINT:
            bsz = 1; break;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
            bsz = 2; break;
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            bsz = 8; break;
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            bsz = 16; break;
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            bw = 4; bh = 4; bsz = 16; break;
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
            bw = 5; bh = 4; bsz = 16; break;
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
            bw = 5; bh = 5; bsz = 16; break;
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
            bw = 6; bh = 5; bsz = 16; break;
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
            bw = 6; bh = 6; bsz = 16; break;
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
            bw = 8; bh = 5; bsz = 16; break;
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
            bw = 8; bh = 6; bsz = 16; break;
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
            bw = 8; bh = 8; bsz = 16; break;
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
            bw = 10; bh = 5; bsz = 16; break;
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
            bw = 10; bh = 6; bsz = 16; break;
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
            bw = 10; bh = 8; bsz = 16; break;
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
            bw = 10; bh = 10; bsz = 16; break;
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
            bw = 12; bh = 10; bsz = 16; break;
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            bw = 12; bh = 12; bsz = 16; break;
        default:
            bw = 1; bh = 1; bsz = 4; break;
    }
    if (out_bw) *out_bw = bw;
    if (out_bh) *out_bh = bh;
    return bsz;
}

void *wrap_swapchain_image_as_mtl_texture(uint32_t sc_idx) {
    MtlVkState &st = get_state();
    if (sc_idx < st.swapchain_tex_objs.size() && st.swapchain_tex_objs[sc_idx]) {
        auto *tex = unwrap_mtl<MtlTextureImpl>(st.swapchain_tex_objs[sc_idx]);
        if (tex) {
            VkImageLayout cur = (sc_idx < st.swapchain_layouts.size()) ? st.swapchain_layouts[sc_idx] : VK_IMAGE_LAYOUT_UNDEFINED;
            tex->mip_layouts.assign(1, cur);
        }
        return st.swapchain_tex_objs[sc_idx];
    }
    return nullptr;
}

namespace {

static std::atomic<uint64_t> g_drawable_id_counter{1};

/* ============================================================================
 * MTLBuffer
 * ============================================================================ */
static int get_buffer_pool_bucket(uint64_t sz, uint64_t *out_cap) {
    static constexpr uint64_t kBuckets[5] = {4096ULL, 16384ULL, 65536ULL, 262144ULL, 1048576ULL};
    for (int i = 0; i < 5; ++i) {
        if (sz <= kBuckets[i]) {
            if (out_cap) *out_cap = kBuckets[i];
            return i;
        }
    }
    if (out_cap) *out_cap = sz;
    return -1;
}

static void mtl_buffer_dealloc(void *self) {
    auto *impl = unwrap_mtl<MtlBufferImpl>(self);
    if (!impl) return;
    MtlVkState &st = get_state();
    const uint64_t alloc_sz = (impl->alloc_capacity > 0) ? impl->alloc_capacity : std::max<uint64_t>(impl->length, 16);
    enqueue_deferred_buffer(impl->buffer, impl->allocation, impl->mapped, impl->alloc_capacity);
    st.live_buffers.fetch_sub(1, std::memory_order_relaxed);
    st.live_buffer_bytes.fetch_sub(static_cast<int64_t>(alloc_sz), std::memory_order_relaxed);
    delete impl;
}

static void *mtl_device_newBufferWithLength(void *self, const char *sel, uint64_t length, uint64_t options) {
    (void)self; (void)sel;
    MtlVkState &st = get_state();
    if (!st.initialized) return nullptr;

    uint64_t alloc_sz = std::max<uint64_t>(length, 16);
    uint64_t bucket_cap = alloc_sz;
    int bucket_idx = get_buffer_pool_bucket(alloc_sz, &bucket_cap);
    if (bucket_idx >= 0) {
        alloc_sz = bucket_cap;
        std::lock_guard<std::mutex> lk(st.mutex);
        auto &pool = st.buffer_pool[bucket_idx];
        if (!pool.empty()) {
            auto pb = pool.back();
            pool.pop_back();
            auto *impl = new MtlBufferImpl();
            impl->buffer = pb.buffer;
            impl->allocation = pb.allocation;
            impl->length = length;
            impl->alloc_capacity = pb.capacity;
            impl->unique_id = next_mtl_resource_uid();
            impl->options = options;
            impl->mapped = pb.mapped;
            impl->host_coherent = true;
            st.live_buffers.fetch_add(1, std::memory_order_relaxed);
            st.live_buffer_bytes.fetch_add(static_cast<int64_t>(pb.capacity), std::memory_order_relaxed);
            return alloc_mtl_wrap_managed("MTLBuffer", impl, &mtl_buffer_dealloc, false);
        }
    }

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = alloc_sz;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo vai{};
    vai.usage = VMA_MEMORY_USAGE_AUTO;
    vai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation valloc = VK_NULL_HANDLE;
    VmaAllocationInfo vinfo{};
    if (vmaCreateBuffer(st.vk_ctx.allocator(), &bci, &vai, &buf, &valloc, &vinfo) != VK_SUCCESS) {
        MTLVK_LOGE("vmaCreateBuffer failed (len=%llu)", (unsigned long long)alloc_sz);
        return nullptr;
    }

    auto *impl = new MtlBufferImpl();
    impl->buffer = buf;
    impl->allocation = valloc;
    impl->length = length;
    impl->alloc_capacity = (bucket_idx >= 0) ? alloc_sz : 0;
    impl->unique_id = next_mtl_resource_uid();
    impl->options = options;
    impl->mapped = vinfo.pMappedData;
    impl->host_coherent = true;
    st.live_buffers.fetch_add(1, std::memory_order_relaxed);
    st.live_buffer_bytes.fetch_add(static_cast<int64_t>(alloc_sz), std::memory_order_relaxed);
    return alloc_mtl_wrap_managed("MTLBuffer", impl, &mtl_buffer_dealloc, false);
}

static void *mtl_device_newBufferWithBytes(void *self, const char *sel, const void *ptr, uint64_t length, uint64_t options) {
    void *buf_obj = mtl_device_newBufferWithLength(self, sel, length, options);
    if (!buf_obj) return nullptr;
    auto *impl = unwrap_mtl<MtlBufferImpl>(buf_obj);
    if (impl && impl->mapped && ptr && length > 0) {
        std::memcpy(impl->mapped, ptr, (size_t)length);
        vmaFlushAllocation(get_state().vk_ctx.allocator(), impl->allocation, 0, length);
        impl->mod_gen.fetch_add(1, std::memory_order_relaxed);
        if (!impl->buffer_views.empty()) {
            get_state().any_buf_tex_dirty.store(true, std::memory_order_release);
        }
    }
    return buf_obj;
}

static void *mtl_device_newBufferNoCopy(void *self, const char *sel, void *ptr, uint64_t length, uint64_t options, void *dealloc) {
    (void)dealloc;
    return mtl_device_newBufferWithBytes(self, sel, ptr, length, options);
}

static void *mtl_buffer_contents(void *self, const char *sel) {
    (void)sel;
    auto *impl = unwrap_mtl<MtlBufferImpl>(self);
    if (impl) {
        if (impl->needs_host_invalidate.exchange(false, std::memory_order_acq_rel) && impl->allocation) {
            vmaInvalidateAllocation(get_state().vk_ctx.allocator(), impl->allocation, 0, VK_WHOLE_SIZE);
        }
        impl->mod_gen.fetch_add(1, std::memory_order_relaxed);
        if (!impl->buffer_views.empty()) {
            get_state().any_buf_tex_dirty.store(true, std::memory_order_release);
        }
    }
    return impl ? impl->mapped : nullptr;
}

static uint64_t mtl_buffer_length(void *self, const char *sel) {
    (void)sel;
    auto *impl = unwrap_mtl<MtlBufferImpl>(self);
    return impl ? impl->length : 0;
}

static void upload_buffer_backed_texture_cpu(MtlTextureImpl *tex, MtlBufferImpl *buf);

static void mtl_buffer_didModifyRange(void *self, const char *sel, uint64_t loc, uint64_t len) {
    (void)sel;
    auto *impl = unwrap_mtl<MtlBufferImpl>(self);
    if (impl && impl->allocation && len > 0) {
        vmaFlushAllocation(get_state().vk_ctx.allocator(), impl->allocation, loc, len);
        impl->mod_gen.fetch_add(1, std::memory_order_relaxed);
        if (!impl->buffer_views.empty()) {
            get_state().any_buf_tex_dirty.store(true, std::memory_order_release);
        }
    }
}

static uint64_t mtl_buffer_storageMode(void *self, const char *sel) {
    (void)sel;
    auto *impl = unwrap_mtl<MtlBufferImpl>(self);
    return impl ? ((impl->options >> 4) & 0xFULL) : 0ULL;
}
static void *mtl_buffer_device(void *self, const char *sel) { (void)self; (void)sel; return get_state().device_obj; }
static void *mtl_buffer_heap(void *self, const char *sel) { (void)self; (void)sel; return nullptr; }
static uint64_t mtl_buffer_cpuCacheMode(void *self, const char *sel) {
    (void)sel;
    auto *impl = unwrap_mtl<MtlBufferImpl>(self);
    return impl ? ((impl->options & 0xFULL) == 1 ? 1ULL : 0ULL) : 0ULL;
}
static void *mtl_buffer_label(void *self, const char *sel) {
    (void)sel;
    auto *impl = unwrap_mtl<MtlBufferImpl>(self);
    return oh2_objc_create_string((impl && !impl->label.empty()) ? impl->label.c_str() : "MTLBuffer");
}
static void mtl_buffer_setLabel(void *self, const char *sel, void *lbl) {
    (void)sel; (void)lbl;
    (void)self;
}
static uint64_t mtl_buffer_setPurgeableState(void *self, const char *sel, uint64_t st) {
    (void)self; (void)sel;
    return (st == 1) ? 2ULL : st;
}
static uint64_t mtl_buffer_allocatedSize(void *self, const char *sel) {
    (void)sel;
    auto *impl = unwrap_mtl<MtlBufferImpl>(self);
    return impl ? impl->length : 0ULL;
}
static void mtl_buffer_makeAliasable(void *self, const char *sel) { (void)self; (void)sel; }
static uint8_t mtl_buffer_isAliasable(void *self, const char *sel) { (void)self; (void)sel; return 0; }
static void mtl_buffer_addDebugMarker(void *self, const char *sel, void *marker, NSRange64 range) { (void)self; (void)sel; (void)marker; (void)range; }
static void mtl_buffer_removeDebugMarkers(void *self, const char *sel) { (void)self; (void)sel; }

/* ============================================================================
 * MTLTextureDescriptor & MTLTexture
 * ============================================================================ */
static void mtl_td_dealloc(void *self) {
    delete unwrap_mtl<MtlTextureDescriptorImpl>(self);
}

static void mtl_tex_dealloc(void *self) {
    auto *tex = unwrap_mtl<MtlTextureImpl>(self);
    if (!tex || tex->is_swapchain) return;
    MtlVkState &st = get_state();
    if (tex->ds_att_view != VK_NULL_HANDLE) {
        enqueue_deferred_view(tex->ds_att_view);
        tex->ds_att_view = VK_NULL_HANDLE;
    }
    for (const auto &sv : tex->subres_att_views) {
        if (sv.view != VK_NULL_HANDLE) enqueue_deferred_view(sv.view);
    }
    tex->subres_att_views.clear();
    if (tex->owns_image) {
        enqueue_deferred_image(tex->image, tex->allocation, tex->owns_view ? tex->view : VK_NULL_HANDLE);
        st.live_textures.fetch_sub(1, std::memory_order_relaxed);
        st.live_texture_bytes.fetch_sub(static_cast<int64_t>(tex->allocated_size), std::memory_order_relaxed);
    } else if (tex->owns_view && tex->view != VK_NULL_HANDLE) {
        enqueue_deferred_view(tex->view);
    }
    if (tex->retains_parent_tex && tex->parent_tex_obj) {
        oh2_objc_release_object(tex->parent_tex_obj);
    }
    if (tex->retains_parent_buf && tex->parent_buffer_obj) {
        for (uint32_t fi = 0; fi < kMaxFramesInFlight; ++fi) {
            auto &pl = st.frames[fi].preamble_btex_list;
            pl.erase(std::remove(pl.begin(), pl.end(), tex), pl.end());
        }
        {
            std::lock_guard<std::mutex> blk(st.buf_tex_mutex);
            auto &ab = st.active_buf_textures;
            ab.erase(std::remove(ab.begin(), ab.end(), tex), ab.end());
        }
        if (auto *pbuf = unwrap_mtl<MtlBufferImpl>(tex->parent_buffer_obj)) {
            auto &bv = pbuf->buffer_views;
            bv.erase(std::remove(bv.begin(), bv.end(), tex), bv.end());
        }
        oh2_objc_release_object(tex->parent_buffer_obj);
    }
    delete tex;
}

static void *mtl_texdesc_alloc_init(uint64_t type, uint64_t fmt, uint64_t w, uint64_t h, uint8_t mipmapped) {
    auto *d = new MtlTextureDescriptorImpl();
    d->texture_type = type;
    d->pixel_format = fmt;
    d->width = (uint32_t)std::max<uint64_t>(1, w);
    d->height = (uint32_t)std::max<uint64_t>(1, h);
    d->depth = 1;
    d->array_length = (type == kMTLTextureTypeCube || type == kMTLTextureTypeCubeArray) ? 6 : 1;
    if (mipmapped) {
        uint32_t m = std::max(d->width, d->height);
        d->mipmap_level_count = 1u + (uint32_t)std::floor(std::log2((double)m));
    } else {
        d->mipmap_level_count = 1;
    }
    return alloc_mtl_wrap_managed("MTLTextureDescriptor", d, &mtl_td_dealloc, true);
}

static void *mtl_texdesc_init(void *self, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlTextureDescriptorImpl> *>(self);
    if (w && !w->impl) w->impl = new MtlTextureDescriptorImpl();
    if (w) oh2_objc_set_dealloc(w, &mtl_td_dealloc);
    return self;
}

static void *mtl_texdesc_copy(void *self, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlTextureDescriptorImpl>(self);
    auto *cp = src ? new MtlTextureDescriptorImpl(*src) : new MtlTextureDescriptorImpl();
    return alloc_mtl_wrap_managed("MTLTextureDescriptor", cp, &mtl_td_dealloc, false);
}

static void *mtl_texdesc_copyWithZone(void *self, const char *sel, void *z) {
    (void)z;
    return mtl_texdesc_copy(self, sel);
}

static void *mtl_texdesc_2d(void *cls, const char *sel, uint64_t fmt, uint64_t w, uint64_t h, uint8_t mipmapped) {
    (void)cls; (void)sel;
    return mtl_texdesc_alloc_init(kMTLTextureType2D, fmt, w, h, mipmapped);
}

static void *mtl_texdesc_cube(void *cls, const char *sel, uint64_t fmt, uint64_t size, uint8_t mipmapped) {
    (void)cls; (void)sel;
    return mtl_texdesc_alloc_init(kMTLTextureTypeCube, fmt, size, size, mipmapped);
}

static uint64_t mtl_td_get_type(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->texture_type : 2; }
static void mtl_td_set_type(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->texture_type = v; }
static uint64_t mtl_td_get_fmt(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->pixel_format : 80; }
static void mtl_td_set_fmt(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->pixel_format = v; }
static uint64_t mtl_td_get_w(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->width : 1; }
static void mtl_td_set_w(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->width = (uint32_t)std::max<uint64_t>(1, v); }
static uint64_t mtl_td_get_h(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->height : 1; }
static void mtl_td_set_h(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->height = (uint32_t)std::max<uint64_t>(1, v); }
static uint64_t mtl_td_get_d(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->depth : 1; }
static void mtl_td_set_d(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->depth = (uint32_t)std::max<uint64_t>(1, v); }
static uint64_t mtl_td_get_mip(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->mipmap_level_count : 1; }
static void mtl_td_set_mip(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->mipmap_level_count = (uint32_t)std::max<uint64_t>(1, v); }
static uint64_t mtl_td_get_samp(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->sample_count : 1; }
static void mtl_td_set_samp(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->sample_count = (uint32_t)std::max<uint64_t>(1, v); }
static uint64_t mtl_td_get_arr(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->array_length : 1; }
static void mtl_td_set_arr(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->array_length = (uint32_t)std::max<uint64_t>(1, v); }
static uint64_t mtl_td_get_opts(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->resource_options : 0; }
static void mtl_td_set_opts(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->resource_options = v; }
static uint64_t mtl_td_get_sm(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->storage_mode : 2; }
static void mtl_td_set_sm(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->storage_mode = v; }
static uint64_t mtl_td_get_cpu(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->cpu_cache_mode : 0; }
static void mtl_td_set_cpu(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->cpu_cache_mode = v; }
static uint64_t mtl_td_get_usg(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->usage : 1; }
static void mtl_td_set_usg(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->usage = v; }
static uint8_t mtl_td_get_optz(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s); return d ? d->allow_gpu_optimized : 1; }
static void mtl_td_set_optz(void *s, const char *sel, uint8_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(s)) d->allow_gpu_optimized = v; }

static VkImageViewType mtl_type_to_vk_view(uint64_t t) {
    switch (t) {
        case kMTLTextureType1D:        return VK_IMAGE_VIEW_TYPE_1D;
        case kMTLTextureType1DArray:   return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case kMTLTextureType2DArray:   return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case kMTLTextureTypeCube:      return VK_IMAGE_VIEW_TYPE_CUBE;
        case kMTLTextureTypeCubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case kMTLTextureType3D:        return VK_IMAGE_VIEW_TYPE_3D;
        default:                       return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static void *mtl_device_newTexture(void *self, const char *sel, void *desc_obj) {
    (void)self; (void)sel;
    MtlVkState &st = get_state();
    if (!st.initialized || !desc_obj) return nullptr;
    auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(desc_obj);
    if (!d) return nullptr;

    bool is_astc = false, is_depth = false, has_stencil = false;
    VkFormat vk_fmt = mtl_pixel_format_to_vk(d->pixel_format, &is_astc, &is_depth, &has_stencil);

    uint32_t layers = d->array_length;
    if (d->texture_type == kMTLTextureTypeCube) layers = 6;
    else if (d->texture_type == kMTLTextureTypeCubeArray) layers = std::max(6u, d->array_length * 6u);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!is_astc) {
        if (is_depth || has_stencil) {
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        } else {
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if ((d->usage & 0x04ULL) != 0 || (d->usage & 0x02ULL) != 0) {
                /* MTLTextureUsageShaderWrite = 0x02 (and RenderTarget = 0x04); add STORAGE_BIT when UNORM/FLOAT supports it */
                if ((d->usage & 0x02ULL) != 0 && vk_fmt != VK_FORMAT_B8G8R8A8_SRGB && vk_fmt != VK_FORMAT_R8G8B8A8_SRGB && vk_fmt != VK_FORMAT_B8G8R8A8_UNORM) {
                    usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                }
            }
        }
    }

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = (d->texture_type == kMTLTextureType3D) ? VK_IMAGE_TYPE_3D :
                    (d->texture_type == kMTLTextureType1D || d->texture_type == kMTLTextureType1DArray) ? VK_IMAGE_TYPE_1D :
                    VK_IMAGE_TYPE_2D;
    if (d->texture_type == kMTLTextureTypeCube || d->texture_type == kMTLTextureTypeCubeArray) {
        ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    ici.format = vk_fmt;
    ici.extent = {std::max(1u, d->width), std::max(1u, d->height), (d->texture_type == kMTLTextureType3D) ? std::max(1u, d->depth) : 1u};
    ici.mipLevels = std::max(1u, d->mipmap_level_count);
    ici.arrayLayers = std::max(1u, layers);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo vai{};
    vai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImage img = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    if (vmaCreateImage(st.vk_ctx.allocator(), &ici, &vai, &img, &alloc, nullptr) != VK_SUCCESS) {
        MTLVK_LOGE("vmaCreateImage failed (fmt=%d %ux%u)", static_cast<int>(vk_fmt), ici.extent.width, ici.extent.height);
        return nullptr;
    }

    VkImageAspectFlags aspect = 0;
    if (is_depth) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (has_stencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (aspect == 0) aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = img;
    ivci.viewType = mtl_type_to_vk_view(d->texture_type);
    ivci.format = vk_fmt;
    if (d->pixel_format == 1 && vk_fmt == VK_FORMAT_R8_UNORM) {
        ivci.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                           VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
    }
    ivci.subresourceRange.aspectMask = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : aspect;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = ici.mipLevels;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = ici.arrayLayers;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(st.vk_ctx.device(), &ivci, nullptr, &view) != VK_SUCCESS) {
        vmaDestroyImage(st.vk_ctx.allocator(), img, alloc);
        return nullptr;
    }

    if (is_astc) {
        st.astc_textures_created.fetch_add(1, std::memory_order_relaxed);
    }

    auto *tex = new MtlTextureImpl();
    tex->unique_id = next_mtl_resource_uid();
    tex->image = img;
    tex->allocation = alloc;
    tex->view = view;
    tex->vk_format = vk_fmt;
    tex->mtl_pixel_format = d->pixel_format;
    tex->texture_type = d->texture_type;
    tex->width = ici.extent.width;
    tex->height = ici.extent.height;
    tex->depth = ici.extent.depth;
    tex->mip_levels = ici.mipLevels;
    tex->array_layers = ici.arrayLayers;
    tex->sample_count = 1;
    tex->aspect_mask = aspect;
    tex->mip_layouts.assign(static_cast<size_t>(ici.mipLevels) * ici.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED);
    tex->owns_image = true;
    tex->owns_view = true;
    tex->cpu_cache_mode = d->cpu_cache_mode;
    tex->storage_mode = d->storage_mode;
    tex->usage = d->usage;
    tex->framebuffer_only = (d->usage == 0x04ULL) ? 1 : 0;
    tex->allocated_size = (uint64_t)ici.extent.width * ici.extent.height * ici.extent.depth * 4ULL;
    st.live_textures.fetch_add(1, std::memory_order_relaxed);
    st.live_texture_bytes.fetch_add(static_cast<int64_t>(tex->allocated_size), std::memory_order_relaxed);
    return alloc_mtl_wrap_managed("MTLTexture", tex, &mtl_tex_dealloc, false);
}

static uint64_t hash_buffer_tex_slice(const MtlTextureImpl *tex, const MtlBufferImpl *buf) {
    if (!tex || !buf || !buf->mapped || tex->buffer_offset >= buf->length) return 0;
    uint32_t w = std::max(1u, tex->width);
    uint32_t h = std::max(1u, tex->height);
    uint32_t d = std::max(1u, tex->depth);
    uint32_t bw = 1, bh = 1;
    uint32_t bsz = mtl_pixel_format_block_size(tex->vk_format, &bw, &bh);
    uint64_t packed_row = static_cast<uint64_t>((w + bw - 1u) / bw) * bsz;
    uint64_t rows = static_cast<uint64_t>((h + bh - 1u) / bh) * d;
    uint64_t pitch = (tex->buffer_bytes_per_row > 0) ? tex->buffer_bytes_per_row : packed_row;
    uint64_t avail = buf->length - tex->buffer_offset;
    uint64_t span = std::min<uint64_t>(avail, (rows > 1) ? ((rows - 1) * pitch + packed_row) : packed_row);
    const uint8_t *p = static_cast<const uint8_t *>(buf->mapped) + tex->buffer_offset;
    uint64_t hash = 14695981039346656037ULL ^ span;
    uint64_t step = (span > 256) ? (span / 32) : 8;
    if (step < 8) step = 8;
    for (uint64_t off = 0; off + 8 <= span; off += step) {
        uint64_t v = 0;
        std::memcpy(&v, p + off, 8);
        hash ^= v;
        hash *= 1099511628211ULL;
    }
    if (span >= 8) {
        uint64_t tail = 0;
        std::memcpy(&tail, p + span - 8, 8);
        hash ^= tail;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void upload_buffer_backed_texture_cpu(MtlTextureImpl *tex, MtlBufferImpl *buf) {
    MtlVkState &st = get_state();
    if (!tex || !buf || !buf->mapped || tex->image == VK_NULL_HANDLE || !st.initialized) return;
    if (tex->buffer_offset >= buf->length) return;

    uint32_t w = std::max(1u, tex->width);
    uint32_t h = std::max(1u, tex->height);
    uint32_t d = std::max(1u, tex->depth);
    uint32_t bw = 1, bh = 1;
    uint32_t bsz = mtl_pixel_format_block_size(tex->vk_format, &bw, &bh);
    uint32_t bx = (w + bw - 1u) / bw;
    uint32_t by = (h + bh - 1u) / bh;
    uint64_t packed_row = static_cast<uint64_t>(bx) * bsz;
    uint64_t src_row_pitch = (tex->buffer_bytes_per_row > 0) ? tex->buffer_bytes_per_row : packed_row;
    uint64_t total_packed = packed_row * by * d;
    if (total_packed == 0) return;

    uint64_t cur_gen = buf->mod_gen.load(std::memory_order_relaxed);
    uint64_t cur_hash = hash_buffer_tex_slice(tex, buf);

    oh2::vk::StagingAllocation staging{};
    if (!st.staging.allocate(total_packed, std::max<VkDeviceSize>(16, st.vk_ctx.caps().optimal_buffer_copy_offset_alignment), staging)) {
        return;
    }

    const uint8_t *src_base = static_cast<const uint8_t *>(buf->mapped) + tex->buffer_offset;
    uint64_t avail = buf->length - tex->buffer_offset;
    uint8_t *dst_base = static_cast<uint8_t *>(staging.mapped);
    std::memset(dst_base, 0, static_cast<size_t>(total_packed));

    for (uint32_t row = 0; row < by * d; ++row) {
        uint64_t src_off = static_cast<uint64_t>(row) * src_row_pitch;
        if (src_off >= avail) break;
        uint64_t can_copy = std::min<uint64_t>(packed_row, avail - src_off);
        std::memcpy(dst_base + static_cast<uint64_t>(row) * packed_row, src_base + src_off, static_cast<size_t>(can_copy));
    }
    st.staging.flush(staging.offset, total_packed);

    struct BufTexUploadCtx {
        MtlTextureImpl *tex;
        oh2::vk::StagingAllocation staging;
        uint32_t w, h, d;
    } ctx{tex, staging, w, h, d};

    execute_immediate_upload([](VkCommandBuffer cmd, void *ud) {
        auto *c = static_cast<BufTexUploadCtx *>(ud);
        VkImageLayout old_l = c->tex->mip_layouts.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : c->tex->mip_layouts[0];
        transition_image_layout(cmd, c->tex->image, c->tex->aspect_mask,
                                0, 1, 0, 1,
                                old_l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy bic{};
        bic.bufferOffset = c->staging.offset;
        bic.bufferRowLength = 0;
        bic.bufferImageHeight = 0;
        bic.imageSubresource = {c->tex->aspect_mask, 0, 0, 1};
        bic.imageOffset = {0, 0, 0};
        bic.imageExtent = {c->w, c->h, c->d};
        vkCmdCopyBufferToImage(cmd, c->staging.buffer, c->tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

        transition_image_layout(cmd, c->tex->image, c->tex->aspect_mask,
                                0, 1, 0, 1,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!c->tex->mip_layouts.empty()) c->tex->mip_layouts[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }, &ctx);
    tex->synced_buf_gen = cur_gen;
    tex->synced_content_hash = cur_hash;
}

static void *mtl_buffer_newTextureWithDescriptor(void *self, const char *sel, void *desc_obj, uint64_t offset, uint64_t bytesPerRow) {
    (void)sel;
    auto *buf = unwrap_mtl<MtlBufferImpl>(self);
    void *tex_obj = mtl_device_newTexture(get_state().device_obj, "newTextureWithDescriptor:", desc_obj);
    if (!tex_obj) return nullptr;
    auto *tex = unwrap_mtl<MtlTextureImpl>(tex_obj);
    if (tex) {
        tex->parent_buffer_obj = self;
        tex->retains_parent_buf = (self != nullptr);
        if (self) oh2_objc_add_refcount(self, 1);
        tex->buffer_offset = offset;
        tex->buffer_bytes_per_row = bytesPerRow;
        if (buf) {
            buf->buffer_views.push_back(tex);
            std::lock_guard<std::mutex> blk(get_state().buf_tex_mutex);
            get_state().active_buf_textures.push_back(tex);
        }
        if (buf && buf->mapped && tex->width > 0 && tex->height > 0) {
            uint64_t avail = (offset < buf->length) ? (buf->length - offset) : 0;
            int64_t delta = static_cast<int64_t>(avail) - static_cast<int64_t>(tex->allocated_size);
            tex->allocated_size = avail;
            get_state().live_texture_bytes.fetch_add(delta, std::memory_order_relaxed);
            tex->synced_buf_gen = 0;
            tex->synced_content_hash = 0;
            get_state().any_buf_tex_dirty.store(true, std::memory_order_release);
        }
    }
    return tex_obj;
}

static void mtl_tex_replaceRegionSlice(void *self, const char *sel, MTLRegion *region, uint64_t level,
                                       uint64_t slice, const void *bytes, uint64_t bytesPerRow, uint64_t bytesPerImage) {
    (void)sel; (void)bytesPerImage;
    auto *tex = unwrap_mtl<MtlTextureImpl>(self);
    MtlVkState &st = get_state();
    if (!tex || !region || !bytes || !st.initialized) return;
    if (level >= tex->mip_levels || slice >= tex->array_layers) return;

    uint32_t w = (uint32_t)std::max<uint64_t>(1, region->size.width);
    uint32_t h = (uint32_t)std::max<uint64_t>(1, region->size.height);
    uint32_t d = (uint32_t)std::max<uint64_t>(1, region->size.depth);

    uint32_t bw = 1, bh = 1;
    uint32_t bsz = mtl_pixel_format_block_size(tex->vk_format, &bw, &bh);
    uint32_t bx = (w + bw - 1u) / bw;
    uint32_t by = (h + bh - 1u) / bh;
    uint64_t packed_row = (uint64_t)bx * bsz;
    uint64_t row_bytes = (bytesPerRow > 0) ? bytesPerRow : packed_row;
    uint64_t img_bytes = (d > 1 && bytesPerImage > 0) ? bytesPerImage : (row_bytes * by);
    uint64_t packed_total = packed_row * by * d;
    if (packed_total == 0) return;

    /* Async deferred upload onto the next VkCommandBuffer — zero vkWaitForFences GPU pipeline stalls */
    if (packed_total <= 16ULL * 1024ULL * 1024ULL) {
        PendingTexUpload item{};
        item.tex_obj = self;
        item.level = (uint32_t)level;
        item.slice = (uint32_t)slice;
        item.reg = *region;
        item.bytes.resize(static_cast<size_t>(packed_total));
        if (row_bytes == packed_row && (d <= 1 || img_bytes == packed_row * by)) {
            std::memcpy(item.bytes.data(), bytes, static_cast<size_t>(packed_total));
        } else {
            const uint8_t *src = static_cast<const uint8_t *>(bytes);
            uint8_t *dst = item.bytes.data();
            for (uint32_t z = 0; z < d; ++z) {
                const uint8_t *src_slice = src + (uint64_t)z * img_bytes;
                uint8_t *dst_slice = dst + (uint64_t)z * (packed_row * by);
                for (uint32_t row = 0; row < by; ++row) {
                    std::memcpy(dst_slice + (uint64_t)row * packed_row,
                                src_slice + (uint64_t)row * row_bytes,
                                static_cast<size_t>(packed_row));
                }
            }
        }
        oh2_objc_add_refcount(self, 1);
        {
            std::lock_guard<std::mutex> plk(st.pending_tex_mutex);
            st.pending_tex_uploads.push_back(std::move(item));
        }
        st.has_pending_tex_uploads.store(true, std::memory_order_release);
        return;
    }

    oh2::vk::StagingAllocation staging{};
    if (!st.staging.allocate(packed_total, std::max<VkDeviceSize>(16, st.vk_ctx.caps().optimal_buffer_copy_offset_alignment), staging)) {
        MTLVK_LOGE("replaceRegion: staging allocate(%llu) failed", (unsigned long long)packed_total);
        return;
    }
    if (row_bytes == packed_row && (d <= 1 || img_bytes == packed_row * by)) {
        std::memcpy(staging.mapped, bytes, (size_t)packed_total);
    } else {
        const uint8_t *src = static_cast<const uint8_t *>(bytes);
        uint8_t *dst = static_cast<uint8_t *>(staging.mapped);
        for (uint32_t z = 0; z < d; ++z) {
            const uint8_t *src_slice = src + (uint64_t)z * img_bytes;
            uint8_t *dst_slice = dst + (uint64_t)z * (packed_row * by);
            for (uint32_t row = 0; row < by; ++row) {
                std::memcpy(dst_slice + (uint64_t)row * packed_row,
                            src_slice + (uint64_t)row * row_bytes,
                            (size_t)packed_row);
            }
        }
    }
    st.staging.flush(staging.offset, packed_total);

    struct CopyCtx {
        MtlTextureImpl *tex;
        oh2::vk::StagingAllocation staging;
        uint32_t level, slice;
        MTLRegion reg;
    } ctx{tex, staging, (uint32_t)level, (uint32_t)slice, *region};

    execute_immediate_upload([](VkCommandBuffer cmd, void *ud) {
        auto *c = static_cast<CopyCtx *>(ud);
        VkImageLayout old_l = c->tex->get_subres_layout(c->level, c->slice);
        transition_image_layout(cmd, c->tex->image, c->tex->aspect_mask,
                                c->level, 1, c->slice, 1,
                                old_l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy bic{};
        bic.bufferOffset = c->staging.offset;
        bic.bufferRowLength = 0;
        bic.bufferImageHeight = 0;
        bic.imageSubresource.aspectMask = c->tex->aspect_mask;
        bic.imageSubresource.mipLevel = c->level;
        bic.imageSubresource.baseArrayLayer = c->slice;
        bic.imageSubresource.layerCount = 1;
        bic.imageOffset = {(int32_t)c->reg.origin.x, (int32_t)c->reg.origin.y, (int32_t)c->reg.origin.z};
        bic.imageExtent = {(uint32_t)c->reg.size.width, (uint32_t)c->reg.size.height, (uint32_t)std::max<uint64_t>(1, c->reg.size.depth)};

        vkCmdCopyBufferToImage(cmd, c->staging.buffer, c->tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

        transition_image_layout(cmd, c->tex->image, c->tex->aspect_mask,
                                c->level, 1, c->slice, 1,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        c->tex->set_subres_layout(c->level, c->slice, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }, &ctx);
}

static void mtl_tex_replaceRegion2D(void *self, const char *sel, MTLRegion *region, uint64_t level,
                                    const void *bytes, uint64_t bytesPerRow) {
    mtl_tex_replaceRegionSlice(self, sel, region, level, 0, bytes, bytesPerRow, 0);
}

static void *mtl_tex_newViewFmt(void *self, const char *sel, uint64_t fmt) {
    (void)sel;
    auto *src = unwrap_mtl<MtlTextureImpl>(self);
    if (!src) return nullptr;
    auto *vtex = new MtlTextureImpl(*src);
    vtex->unique_id = next_mtl_resource_uid();
    vtex->ds_att_view = VK_NULL_HANDLE;
    vtex->subres_att_views.clear();
    vtex->owns_image = false;
    vtex->is_swapchain = false;
    vtex->parent_tex_obj = self;
    vtex->retains_parent_tex = (self != nullptr);
    vtex->retains_parent_buf = false;
    if (self) oh2_objc_add_refcount(self, 1);
    vtex->mtl_pixel_format = fmt;
    vtex->vk_format = mtl_pixel_format_to_vk(fmt);
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = src->image;
    ivci.viewType = mtl_type_to_vk_view(src->texture_type);
    ivci.format = vtex->vk_format;
    if (fmt == 1 && vtex->vk_format == VK_FORMAT_R8_UNORM) {
        ivci.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                           VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
    }
    VkImageAspectFlags view_aspect = src->aspect_mask;
    if ((view_aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ==
        (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        view_aspect = (fmt == 253 || fmt == 261) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    ivci.subresourceRange = {view_aspect, 0, src->mip_levels, 0, src->array_layers};
    if (vkCreateImageView(get_state().vk_ctx.device(), &ivci, nullptr, &vtex->view) == VK_SUCCESS) {
        vtex->owns_view = true;
    } else {
        vtex->view = src->view;
        vtex->owns_view = false;
    }
    return alloc_mtl_wrap_managed("MTLTexture", vtex, &mtl_tex_dealloc, false);
}

static void *mtl_tex_newViewRange(void *self, const char *sel, uint64_t fmt, uint64_t ttype,
                                  NSRange64 levels, NSRange64 slices) {
    (void)sel;
    auto *src = unwrap_mtl<MtlTextureImpl>(self);
    if (!src) return nullptr;
    auto *vtex = new MtlTextureImpl(*src);
    vtex->unique_id = next_mtl_resource_uid();
    vtex->ds_att_view = VK_NULL_HANDLE;
    vtex->subres_att_views.clear();
    vtex->owns_image = false;
    vtex->is_swapchain = false;
    vtex->parent_tex_obj = self;
    vtex->retains_parent_tex = (self != nullptr);
    vtex->retains_parent_buf = false;
    if (self) oh2_objc_add_refcount(self, 1);
    vtex->texture_type = ttype;
    vtex->mtl_pixel_format = fmt;
    vtex->vk_format = mtl_pixel_format_to_vk(fmt);
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = src->image;
    ivci.viewType = mtl_type_to_vk_view(ttype);
    ivci.format = vtex->vk_format;
    if (fmt == 1 && vtex->vk_format == VK_FORMAT_R8_UNORM) {
        ivci.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                           VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
    }
    VkImageAspectFlags view_aspect = src->aspect_mask;
    if ((view_aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ==
        (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        view_aspect = (fmt == 253 || fmt == 261) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    ivci.subresourceRange = {
        view_aspect,
        (uint32_t)levels.location,
        (uint32_t)std::max<uint64_t>(1, levels.length),
        (uint32_t)slices.location,
        (uint32_t)std::max<uint64_t>(1, slices.length),
    };
    if (vkCreateImageView(get_state().vk_ctx.device(), &ivci, nullptr, &vtex->view) == VK_SUCCESS) {
        vtex->owns_view = true;
    } else {
        vtex->view = src->view;
        vtex->owns_view = false;
    }
    return alloc_mtl_wrap_managed("MTLTexture", vtex, &mtl_tex_dealloc, false);
}

static uint64_t mtl_tex_get_w(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->width : 1; }
static uint64_t mtl_tex_get_h(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->height : 1; }
static uint64_t mtl_tex_get_d(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->depth : 1; }
static uint64_t mtl_tex_get_mip(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->mip_levels : 1; }
static uint64_t mtl_tex_get_arr(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->array_layers : 1; }
static uint64_t mtl_tex_get_fmt(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->mtl_pixel_format : 80; }
static uint64_t mtl_tex_get_type(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->texture_type : 2; }
static uint64_t mtl_tex_get_samp(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->sample_count : 1; }
static void *mtl_tex_parent(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->parent_tex_obj : nullptr; }
static void *mtl_tex_buffer(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->parent_buffer_obj : nullptr; }
static uint64_t mtl_tex_bufferOff(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->buffer_offset : 0ULL; }
static uint64_t mtl_tex_bufferBpr(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->buffer_bytes_per_row : 0ULL; }
static uint64_t mtl_tex_zero(void *s, const char *sel) { (void)s; (void)sel; return 0ULL; }
static void *mtl_tex_rootResource(void *s, const char *sel) {
    (void)sel;
    auto *t = unwrap_mtl<MtlTextureImpl>(s);
    if (t && t->parent_buffer_obj) return t->parent_buffer_obj;
    if (t && t->parent_tex_obj) return t->parent_tex_obj;
    return s;
}
static uint8_t mtl_tex_isFramebufferOnly(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->framebuffer_only : 0; }
static uint64_t mtl_tex_allocatedSize(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->allocated_size : 0ULL; }
static uint64_t mtl_tex_storageMode(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->storage_mode : 2ULL; }
static uint64_t mtl_tex_usage(void *s, const char *sel) { (void)sel; auto *t = unwrap_mtl<MtlTextureImpl>(s); return t ? t->usage : 1ULL; }
static void *mtl_tex_iosurface(void *s, const char *sel) { (void)s; (void)sel; return nullptr; }
static void mtl_tex_getBytesSlice(void *s, const char *sel, void *dst, uint64_t bpr, uint64_t bpi,
                                  MTLRegion *reg, uint64_t level, uint64_t slice) {
    (void)s; (void)sel; (void)dst; (void)bpr; (void)bpi; (void)reg; (void)level; (void)slice;
}
static void mtl_tex_getBytes2D(void *s, const char *sel, void *dst, uint64_t bpr,
                               MTLRegion *reg, uint64_t level) {
    (void)s; (void)sel; (void)dst; (void)bpr; (void)reg; (void)level;
}
static void *mtl_tex_label(void *s, const char *sel) {
    (void)sel;
    auto *t = unwrap_mtl<MtlTextureImpl>(s);
    return oh2_objc_create_string((t && !t->label.empty()) ? t->label.c_str() : "MTLTexture");
}

/* ============================================================================
 * MTLSamplerDescriptor / MTLSamplerState & MTLDepthStencilDescriptor / State
 * ============================================================================ */
static void mtl_sampdesc_dealloc(void *self) {
    delete unwrap_mtl<MtlSamplerDescriptorImpl>(self);
}

static void *mtl_sampdesc_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlSamplerDescriptorImpl> *>(s);
    if (w && !w->impl) w->impl = new MtlSamplerDescriptorImpl();
    if (w) oh2_objc_set_dealloc(w, &mtl_sampdesc_dealloc);
    return s;
}
static void *mtl_sampdesc_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlSamplerDescriptorImpl>(s);
    auto *cp = src ? new MtlSamplerDescriptorImpl(*src) : new MtlSamplerDescriptorImpl();
    return alloc_mtl_wrap_managed("MTLSamplerDescriptor", cp, &mtl_sampdesc_dealloc, false);
}
static void *mtl_sampdesc_copyWithZone(void *s, const char *sel, void *z) {
    (void)z;
    return mtl_sampdesc_copy(s, sel);
}
static uint64_t mtl_sd_get_min(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->min_filter : 0; }
static void mtl_sd_set_min(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->min_filter = v; }
static uint64_t mtl_sd_get_mag(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->mag_filter : 0; }
static void mtl_sd_set_mag(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->mag_filter = v; }
static uint64_t mtl_sd_get_mip(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->mip_filter : 0; }
static void mtl_sd_set_mip(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->mip_filter = v; }
static uint64_t mtl_sd_get_sa(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->s_addr : 0; }
static void mtl_sd_set_sa(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->s_addr = v; }
static uint64_t mtl_sd_get_ta(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->t_addr : 0; }
static void mtl_sd_set_ta(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->t_addr = v; }
static uint64_t mtl_sd_get_ra(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->r_addr : 0; }
static void mtl_sd_set_ra(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->r_addr = v; }
static uint64_t mtl_sd_get_aniso(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->max_anisotropy : 1; }
static void mtl_sd_set_aniso(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->max_anisotropy = v; }
static float mtl_sd_get_lmin(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->lod_min : 0.0f; }
static void mtl_sd_set_lmin(void *s, const char *sel, float v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->lod_min = v; }
static float mtl_sd_get_lmax(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->lod_max : 1000.0f; }
static void mtl_sd_set_lmax(void *s, const char *sel, float v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->lod_max = v; }
static uint64_t mtl_sd_get_cmp(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->compare_func : 0; }
static void mtl_sd_set_cmp(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->compare_func = v; }
static uint64_t mtl_sd_get_border(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->border_color : 0; }
static void mtl_sd_set_border(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->border_color = v; }
static uint8_t mtl_sd_get_norm(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s); return d ? d->normalized_coords : 1; }
static void mtl_sd_set_norm(void *s, const char *sel, uint8_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(s)) d->normalized_coords = v; }
static uint8_t mtl_sd_false(void *s, const char *sel) { (void)s; (void)sel; return 0; }
static void mtl_sd_noop_u8(void *s, const char *sel, uint8_t v) { (void)s; (void)sel; (void)v; }
static void *mtl_sd_label(void *s, const char *sel) { (void)s; (void)sel; return oh2_objc_create_string("MTLSampler"); }

static VkSamplerAddressMode map_addr(uint64_t m) {
    switch (m) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case 2: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 4: /* ClampToZero */
        case 5: /* ClampToBorderColor */
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkBorderColor map_border_color(uint64_t s_addr, uint64_t t_addr, uint64_t r_addr, uint64_t border) {
    if (s_addr == 4 || t_addr == 4 || r_addr == 4) {
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    switch (border) {
        case 1: return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        case 2: return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        default: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
}

[[maybe_unused]] static VkCompareOp map_cmp_op(uint64_t c) {
    switch (c) {
        case 1: return VK_COMPARE_OP_LESS;
        case 2: return VK_COMPARE_OP_EQUAL;
        case 3: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 4: return VK_COMPARE_OP_GREATER;
        case 5: return VK_COMPARE_OP_NOT_EQUAL;
        case 6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case 7: return VK_COMPARE_OP_ALWAYS;
        default: return VK_COMPARE_OP_NEVER;
    }
}

static void mtl_sampler_dealloc(void *self) {
    auto *impl = unwrap_mtl<MtlSamplerImpl>(self);
    if (!impl) return;
    enqueue_deferred_sampler(impl->sampler);
    if (impl->nearest_sampler != VK_NULL_HANDLE && impl->nearest_sampler != impl->sampler) {
        enqueue_deferred_sampler(impl->nearest_sampler);
    }
    delete impl;
}

static void *mtl_device_newSampler(void *self, const char *sel, void *desc_obj) {
    (void)self; (void)sel;
    MtlVkState &st = get_state();
    if (!st.initialized) return nullptr;
    auto *d = unwrap_mtl<MtlSamplerDescriptorImpl>(desc_obj);
    MtlSamplerDescriptorImpl def{};
    if (!d) d = &def;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.minFilter = (d->min_filter == 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.magFilter = (d->mag_filter == 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = (d->mip_filter == 2) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = map_addr(d->s_addr);
    sci.addressModeV = map_addr(d->t_addr);
    sci.addressModeW = map_addr(d->r_addr);
    sci.borderColor = map_border_color(d->s_addr, d->t_addr, d->r_addr, d->border_color);
    sci.maxLod = (d->mip_filter == 0) ? 0.0f : std::max(d->lod_max, 0.25f);
    sci.minLod = d->lod_min;
    if (d->max_anisotropy > 1 && st.vk_ctx.caps().supported_features.samplerAnisotropy) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min<float>((float)d->max_anisotropy, st.vk_ctx.caps().max_sampler_anisotropy);
    }
    /*
     * All 4,544 AIR 2.0 shaders in Oceanhorn 2 bind depth/shadow textures as standard
     * texture2d<float> (0 depth2d bindings) and sample raw depth values via non-Dref
     * OpImageGather / OpImageSample*, performing PCF comparison in shader ALU.
     * In Metal, MTLSamplerDescriptor.compareFunction is ignored for texture2d<float>
     * gather/sample; in Vulkan (VUID-RuntimeSpirv-OpImage-06376), setting compareEnable=VK_TRUE
     * on a sampler used with non-Dref OpImageGather/OpImageSample* causes hardware compare
     * against Dref=0.0, returning 0.0 (100% shadow) across the entire sunlit CSM frustum.
     */
    sci.compareEnable = VK_FALSE;
    sci.compareOp = VK_COMPARE_OP_NEVER;
    VkSampler smp = VK_NULL_HANDLE;
    if (vkCreateSampler(st.vk_ctx.device(), &sci, nullptr, &smp) != VK_SUCCESS) return nullptr;
    VkSampler nearest_smp = smp;
    if (sci.minFilter != VK_FILTER_NEAREST || sci.magFilter != VK_FILTER_NEAREST ||
        sci.mipmapMode != VK_SAMPLER_MIPMAP_MODE_NEAREST || sci.anisotropyEnable != VK_FALSE) {
        VkSamplerCreateInfo nsci = sci;
        nsci.minFilter = VK_FILTER_NEAREST;
        nsci.magFilter = VK_FILTER_NEAREST;
        nsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        nsci.anisotropyEnable = VK_FALSE;
        nsci.maxAnisotropy = 1.0f;
        if (vkCreateSampler(st.vk_ctx.device(), &nsci, nullptr, &nearest_smp) != VK_SUCCESS) {
            nearest_smp = smp;
        }
    }
    auto *impl = new MtlSamplerImpl();
    impl->sampler = smp;
    impl->nearest_sampler = nearest_smp;
    impl->desc = *d;
    return alloc_mtl_wrap_managed("MTLSamplerState", impl, &mtl_sampler_dealloc, false);
}

static void mtl_stencildesc_dealloc(void *self) {
    delete unwrap_mtl<MtlStencilDescriptorImpl>(self);
}

static void mtl_wrap_only_noop_dealloc(void *self) {
    (void)self;
}

static void *mtl_stencildesc_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlStencilDescriptorImpl> *>(s);
    if (w && !w->impl) w->impl = new MtlStencilDescriptorImpl();
    if (w) oh2_objc_set_dealloc(w, &mtl_stencildesc_dealloc);
    return s;
}
static void *mtl_stencildesc_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlStencilDescriptorImpl>(s);
    auto *cp = src ? new MtlStencilDescriptorImpl(*src) : new MtlStencilDescriptorImpl();
    return alloc_mtl_wrap_managed("MTLStencilDescriptor", cp, &mtl_stencildesc_dealloc, false);
}
static void *mtl_stencildesc_copyWithZone(void *s, const char *sel, void *z) {
    (void)z;
    return mtl_stencildesc_copy(s, sel);
}
static void mtl_st_cmp(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s)) d->compare_fn = v; }
static void mtl_st_sfail(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s)) d->stencil_fail_op = v; }
static void mtl_st_dfail(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s)) d->depth_fail_op = v; }
static void mtl_st_pass(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s)) d->depth_stencil_pass_op = v; }
static void mtl_st_rmask(void *s, const char *sel, uint32_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s)) d->read_mask = v; }
static void mtl_st_wmask(void *s, const char *sel, uint32_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s)) d->write_mask = v; }

static void mtl_dsdesc_dealloc(void *self) {
    auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(self);
    if (!d) return;
    if (d->front_wrap) oh2_objc_release_object(d->front_wrap);
    if (d->back_wrap) oh2_objc_release_object(d->back_wrap);
    delete d;
}

static void *mtl_dsdesc_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlDepthStencilDescriptorImpl> *>(s);
    if (w && !w->impl) w->impl = new MtlDepthStencilDescriptorImpl();
    if (w) oh2_objc_set_dealloc(w, &mtl_dsdesc_dealloc);
    return s;
}
static void *mtl_dsdesc_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s);
    auto *cp = src ? new MtlDepthStencilDescriptorImpl(*src) : new MtlDepthStencilDescriptorImpl();
    cp->front_wrap = nullptr;
    cp->back_wrap = nullptr;
    return alloc_mtl_wrap_managed("MTLDepthStencilDescriptor", cp, &mtl_dsdesc_dealloc, false);
}
static void *mtl_dsdesc_copyWithZone(void *s, const char *sel, void *z) {
    (void)z;
    return mtl_dsdesc_copy(s, sel);
}
static void mtl_ds_set_cmp(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s)) d->depth_compare_fn = v; }
static void mtl_ds_set_wr(void *s, const char *sel, uint8_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s)) d->depth_write_enabled = v; }
static uint8_t mtl_ds_get_wr(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s); return d ? d->depth_write_enabled : 0; }
static uint64_t mtl_ds_get_cmp(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s); return d ? d->depth_compare_fn : 7; }
static void mtl_ds_set_front(void *s, const char *sel, void *st_obj) {
    (void)sel;
    auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s);
    auto *sd = unwrap_mtl<MtlStencilDescriptorImpl>(st_obj);
    if (d && sd) d->front = *sd;
}
static void mtl_ds_set_back(void *s, const char *sel, void *st_obj) {
    (void)sel;
    auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s);
    auto *sd = unwrap_mtl<MtlStencilDescriptorImpl>(st_obj);
    if (d && sd) d->back = *sd;
}

static void *mtl_device_newDepthStencilState(void *self, const char *sel, void *desc_obj) {
    (void)self; (void)sel;
    auto *impl = new MtlDepthStencilStateImpl();
    if (desc_obj) {
        auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(desc_obj);
        if (d) {
            impl->desc = *d;
            impl->desc.front_wrap = nullptr;
            impl->desc.back_wrap = nullptr;
        }
    }
    return alloc_mtl_wrap("MTLDepthStencilState", impl);
}

/* ============================================================================
 * CAMetalLayer & CAMetalDrawable
 * ============================================================================ */
static MtlMetalLayerImpl *ensure_layer_impl(void *self) {
    auto *w = static_cast<MtlObjWrap<MtlMetalLayerImpl> *>(self);
    if (!w) return nullptr;
    if (!w->impl) {
        w->impl = new MtlMetalLayerImpl();
        MtlVkState &st = get_state();
        VkExtent2D ext = st.vk_ctx.swapchain_extent();
        w->impl->drawable_width = (double)(st.virtual_render_w > 0 ? st.virtual_render_w : ext.width);
        w->impl->drawable_height = (double)(st.virtual_render_h > 0 ? st.virtual_render_h : ext.height);
    }
    oh2_objc_mark_immortal(self);
    get_state().shared_layer_obj = self;
    return w->impl;
}

static void *mtl_layer_init(void *s, const char *sel) {
    (void)sel;
    ensure_layer_impl(s);
    return s;
}
static void *mtl_layer_get_dev(void *s, const char *sel) {
    (void)sel;
    auto *l = ensure_layer_impl(s);
    if (l && l->device_obj) return l->device_obj;
    return get_state().device_obj;
}
static void mtl_layer_set_dev(void *s, const char *sel, void *dev) {
    (void)sel;
    if (auto *l = ensure_layer_impl(s)) l->device_obj = dev;
}
static uint64_t mtl_layer_get_fmt(void *s, const char *sel) {
    (void)sel;
    auto *l = ensure_layer_impl(s);
    return l ? l->pixel_format : 80;
}
static void mtl_layer_set_fmt(void *s, const char *sel, uint64_t fmt) {
    (void)sel;
    if (auto *l = ensure_layer_impl(s)) l->pixel_format = fmt;
}
static uint8_t mtl_layer_get_fbo(void *s, const char *sel) {
    (void)sel;
    auto *l = ensure_layer_impl(s);
    return l ? l->framebuffer_only : 1;
}
static void mtl_layer_set_fbo(void *s, const char *sel, uint8_t v) {
    (void)sel;
    if (auto *l = ensure_layer_impl(s)) l->framebuffer_only = v;
}
static CGSize64 mtl_layer_get_size(void *s, const char *sel) {
    (void)sel;
    auto *l = ensure_layer_impl(s);
    return l ? CGSize64{l->drawable_width, l->drawable_height} : CGSize64{2340.0, 1080.0};
}
static void mtl_layer_set_size(void *s, const char *sel, CGSize64 sz) {
    (void)sel;
    if (auto *l = ensure_layer_impl(s)) {
        l->drawable_width = sz.width;
        l->drawable_height = sz.height;
    }
}
static void mtl_layer_set_scale(void *s, const char *sel, double sc) {
    (void)sel;
    if (auto *l = ensure_layer_impl(s)) l->contents_scale = sc;
}
static void mtl_layer_set_tx(void *s, const char *sel, uint8_t v) {
    (void)sel;
    if (auto *l = ensure_layer_impl(s)) l->presents_with_tx = v;
}
static void mtl_layer_noop_u8(void *s, const char *sel, uint8_t v) {
    (void)sel; (void)v;
    ensure_layer_impl(s);
}
static void mtl_layer_noop_ptr(void *s, const char *sel, void *p) {
    (void)sel; (void)p;
    ensure_layer_impl(s);
}
struct MtlRect64 { double x, y, width, height; };
static MtlRect64 mtl_layer_get_bounds(void *s, const char *sel) {
    (void)sel;
    auto *l = ensure_layer_impl(s);
    return l ? MtlRect64{0.0, 0.0, l->drawable_width, l->drawable_height} : MtlRect64{0.0, 0.0, 2340.0, 1080.0};
}
static void mtl_layer_set_bounds(void *s, const char *sel, MtlRect64 r) {
    (void)sel; (void)r;
    ensure_layer_impl(s);
}

static void *mtl_layer_nextDrawable(void *self, const char *sel) {
    (void)sel;
    auto *l = ensure_layer_impl(self);
    if (!l) return nullptr;
    if (l->has_active_drawable && l->active_drawable_obj) {
        return l->active_drawable_obj;
    }

    uint32_t sc_idx = 0;
    VkSemaphore wait_sem = VK_NULL_HANDLE;
    uint64_t surf_gen = 0;
    if (!mtl_acquire_swapchain_image_internal(l, &sc_idx, &wait_sem, &surf_gen)) {
        return nullptr;
    }

    MtlVkState &st = get_state();
    if (sc_idx >= st.swapchain_drawable_objs.size() || !st.swapchain_drawable_objs[sc_idx]) {
        return nullptr;
    }
    void *dr_obj = st.swapchain_drawable_objs[sc_idx];
    auto *dr = unwrap_mtl<MtlDrawableImpl>(dr_obj);
    dr->layer_obj = self;
    dr->swapchain_idx = sc_idx;
    dr->drawable_id = g_drawable_id_counter.fetch_add(1, std::memory_order_relaxed);
    dr->acquire_sem = wait_sem;
    dr->surface_gen = surf_gen;
    dr->acquired = true;
    dr->texture_obj = wrap_swapchain_image_as_mtl_texture(sc_idx);

    l->has_active_drawable = true;
    l->active_sc_idx = sc_idx;
    l->active_drawable_obj = dr_obj;
    return dr_obj;
}

static void *mtl_drawable_texture(void *self, const char *sel) {
    (void)sel;
    auto *dr = unwrap_mtl<MtlDrawableImpl>(self);
    return dr ? dr->texture_obj : nullptr;
}

static uint64_t mtl_drawable_id(void *self, const char *sel) {
    (void)sel;
    auto *dr = unwrap_mtl<MtlDrawableImpl>(self);
    return dr ? dr->drawable_id : 0;
}

static void *mtl_drawable_layer(void *self, const char *sel) {
    (void)sel;
    auto *dr = unwrap_mtl<MtlDrawableImpl>(self);
    return dr ? dr->layer_obj : nullptr;
}

static void mtl_drawable_present(void *self, const char *sel) {
    (void)self; (void)sel;
}

} // namespace

void register_resource_objc_methods() {
    oh2_objc_ensure_class("MTLBuffer", "NSObject");
    oh2_objc_ensure_class("MTLTexture", "NSObject");
    oh2_objc_ensure_class("MTLSamplerState", "NSObject");
    oh2_objc_ensure_class("MTLDepthStencilState", "NSObject");
    oh2_objc_ensure_class("CAMetalDrawable", "NSObject");

    oh2_objc_register_instance_method("MTLDevice", "newBufferWithLength:options:",                       (void *)&mtl_device_newBufferWithLength);
    oh2_objc_register_instance_method("MTLDevice", "newBufferWithBytes:length:options:",                 (void *)&mtl_device_newBufferWithBytes);
    oh2_objc_register_instance_method("MTLDevice", "newBufferWithBytesNoCopy:length:options:deallocator:", (void *)&mtl_device_newBufferNoCopy);
    oh2_objc_register_instance_method("MTLDevice", "newTextureWithDescriptor:",                          (void *)&mtl_device_newTexture);
    oh2_objc_register_instance_method("MTLDevice", "newSamplerStateWithDescriptor:",                     (void *)&mtl_device_newSampler);
    oh2_objc_register_instance_method("MTLDevice", "newDepthStencilStateWithDescriptor:",                (void *)&mtl_device_newDepthStencilState);

    oh2_objc_register_instance_method("MTLBuffer", "contents",                                           (void *)&mtl_buffer_contents);
    oh2_objc_register_instance_method("MTLBuffer", "length",                                             (void *)&mtl_buffer_length);
    oh2_objc_register_instance_method("MTLBuffer", "didModifyRange:",                                    (void *)&mtl_buffer_didModifyRange);
    oh2_objc_register_instance_method("MTLBuffer", "storageMode",                                        (void *)&mtl_buffer_storageMode);
    oh2_objc_register_instance_method("MTLBuffer", "device",                                             (void *)&mtl_buffer_device);
    oh2_objc_register_instance_method("MTLBuffer", "heap",                                               (void *)&mtl_buffer_heap);
    oh2_objc_register_instance_method("MTLBuffer", "cpuCacheMode",                                       (void *)&mtl_buffer_cpuCacheMode);
    oh2_objc_register_instance_method("MTLBuffer", "label",                                              (void *)&mtl_buffer_label);
    oh2_objc_register_instance_method("MTLBuffer", "setLabel:",                                          (void *)&mtl_buffer_setLabel);
    oh2_objc_register_instance_method("MTLBuffer", "setPurgeableState:",                                 (void *)&mtl_buffer_setPurgeableState);
    oh2_objc_register_instance_method("MTLBuffer", "allocatedSize",                                      (void *)&mtl_buffer_allocatedSize);
    oh2_objc_register_instance_method("MTLBuffer", "makeAliasable",                                      (void *)&mtl_buffer_makeAliasable);
    oh2_objc_register_instance_method("MTLBuffer", "isAliasable",                                        (void *)&mtl_buffer_isAliasable);
    oh2_objc_register_instance_method("MTLBuffer", "addDebugMarker:range:",                              (void *)&mtl_buffer_addDebugMarker);
    oh2_objc_register_instance_method("MTLBuffer", "removeAllDebugMarkers",                              (void *)&mtl_buffer_removeDebugMarkers);
    oh2_objc_register_instance_method("MTLBuffer", "newTextureWithDescriptor:offset:bytesPerRow:",       (void *)&mtl_buffer_newTextureWithDescriptor);

    oh2_objc_register_class_method("MTLTextureDescriptor", "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", (void *)&mtl_texdesc_2d);
    oh2_objc_register_class_method("MTLTextureDescriptor", "textureCubeDescriptorWithPixelFormat:size:mipmapped:",        (void *)&mtl_texdesc_cube);
    static auto mtl_texdesc_buf = +[](void *cls, const char *sel, uint64_t fmt, uint64_t w, uint64_t opts, uint64_t usg) -> void * {
        void *td = mtl_texdesc_2d(cls, sel, fmt, w, 1ULL, 0);
        if (auto *d = unwrap_mtl<MtlTextureDescriptorImpl>(td)) {
            d->texture_type = 0 /* 1D/buffer fallback */;
            d->resource_options = opts;
            d->storage_mode = (opts >> 4) & 0xFULL;
            d->cpu_cache_mode = opts & 0xFULL;
            d->usage = usg;
        }
        return td;
    };
    oh2_objc_register_class_method("MTLTextureDescriptor", "textureBufferDescriptorWithPixelFormat:width:resourceOptions:usage:", (void *)mtl_texdesc_buf);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "init",                        (void *)&mtl_texdesc_init);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "copy",                        (void *)&mtl_texdesc_copy);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "copyWithZone:",               (void *)&mtl_texdesc_copyWithZone);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "textureType",                 (void *)&mtl_td_get_type);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setTextureType:",             (void *)&mtl_td_set_type);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "pixelFormat",                 (void *)&mtl_td_get_fmt);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setPixelFormat:",             (void *)&mtl_td_set_fmt);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "width",                       (void *)&mtl_td_get_w);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setWidth:",                   (void *)&mtl_td_set_w);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "height",                      (void *)&mtl_td_get_h);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setHeight:",                  (void *)&mtl_td_set_h);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "depth",                       (void *)&mtl_td_get_d);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setDepth:",                   (void *)&mtl_td_set_d);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "mipmapLevelCount",            (void *)&mtl_td_get_mip);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setMipmapLevelCount:",        (void *)&mtl_td_set_mip);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "sampleCount",                 (void *)&mtl_td_get_samp);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setSampleCount:",             (void *)&mtl_td_set_samp);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "arrayLength",                 (void *)&mtl_td_get_arr);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setArrayLength:",             (void *)&mtl_td_set_arr);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "resourceOptions",             (void *)&mtl_td_get_opts);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setResourceOptions:",         (void *)&mtl_td_set_opts);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "storageMode",                 (void *)&mtl_td_get_sm);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setStorageMode:",             (void *)&mtl_td_set_sm);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "cpuCacheMode",                (void *)&mtl_td_get_cpu);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setCpuCacheMode:",            (void *)&mtl_td_set_cpu);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "usage",                       (void *)&mtl_td_get_usg);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setUsage:",                   (void *)&mtl_td_set_usg);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "allowGPUOptimizedContents",   (void *)&mtl_td_get_optz);
    oh2_objc_register_instance_method("MTLTextureDescriptor", "setAllowGPUOptimizedContents:", (void *)&mtl_td_set_optz);

    oh2_objc_register_instance_method("MTLTexture", "width",            (void *)&mtl_tex_get_w);
    oh2_objc_register_instance_method("MTLTexture", "height",           (void *)&mtl_tex_get_h);
    oh2_objc_register_instance_method("MTLTexture", "depth",            (void *)&mtl_tex_get_d);
    oh2_objc_register_instance_method("MTLTexture", "mipmapLevelCount", (void *)&mtl_tex_get_mip);
    oh2_objc_register_instance_method("MTLTexture", "arrayLength",      (void *)&mtl_tex_get_arr);
    oh2_objc_register_instance_method("MTLTexture", "pixelFormat",      (void *)&mtl_tex_get_fmt);
    oh2_objc_register_instance_method("MTLTexture", "textureType",      (void *)&mtl_tex_get_type);
    oh2_objc_register_instance_method("MTLTexture", "sampleCount",      (void *)&mtl_tex_get_samp);
    oh2_objc_register_instance_method("MTLTexture", "parentTexture",    (void *)&mtl_tex_parent);
    oh2_objc_register_instance_method("MTLTexture", "device",           (void *)&mtl_buffer_device);
    oh2_objc_register_instance_method("MTLTexture", "heap",             (void *)&mtl_buffer_heap);
    oh2_objc_register_instance_method("MTLTexture", "cpuCacheMode",     (void *)&mtl_tex_zero);
    oh2_objc_register_instance_method("MTLTexture", "label",            (void *)&mtl_tex_label);
    oh2_objc_register_instance_method("MTLTexture", "setLabel:",        (void *)&mtl_buffer_setLabel);
    oh2_objc_register_instance_method("MTLTexture", "setPurgeableState:", (void *)&mtl_buffer_setPurgeableState);
    oh2_objc_register_instance_method("MTLTexture", "allocatedSize",    (void *)&mtl_tex_allocatedSize);
    oh2_objc_register_instance_method("MTLTexture", "buffer",           (void *)&mtl_tex_buffer);
    oh2_objc_register_instance_method("MTLTexture", "bufferOffset",     (void *)&mtl_tex_bufferOff);
    oh2_objc_register_instance_method("MTLTexture", "bufferBytesPerRow", (void *)&mtl_tex_bufferBpr);
    oh2_objc_register_instance_method("MTLTexture", "parentRelativeLevel", (void *)&mtl_tex_zero);
    oh2_objc_register_instance_method("MTLTexture", "parentRelativeSlice", (void *)&mtl_tex_zero);
    oh2_objc_register_instance_method("MTLTexture", "rootResource",     (void *)&mtl_tex_rootResource);
    oh2_objc_register_instance_method("MTLTexture", "makeAliasable",    (void *)&mtl_buffer_makeAliasable);
    oh2_objc_register_instance_method("MTLTexture", "isAliasable",      (void *)&mtl_buffer_isAliasable);
    oh2_objc_register_instance_method("MTLTexture", "isFramebufferOnly", (void *)&mtl_tex_isFramebufferOnly);
    oh2_objc_register_instance_method("MTLTexture", "storageMode",      (void *)&mtl_tex_storageMode);
    oh2_objc_register_instance_method("MTLTexture", "usage",            (void *)&mtl_tex_usage);
    oh2_objc_register_instance_method("MTLTexture", "iosurface",        (void *)&mtl_tex_iosurface);
    oh2_objc_register_instance_method("MTLTexture", "iosurfacePlane",   (void *)&mtl_tex_zero);
    oh2_objc_register_instance_method("MTLTexture", "getBytes:bytesPerRow:fromRegion:mipmapLevel:",                  (void *)&mtl_tex_getBytes2D);
    oh2_objc_register_instance_method("MTLTexture", "getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:", (void *)&mtl_tex_getBytesSlice);
    oh2_objc_register_instance_method("MTLTexture", "newTextureViewWithPixelFormat:",                            (void *)&mtl_tex_newViewFmt);
    oh2_objc_register_instance_method("MTLTexture", "newTextureViewWithPixelFormat:textureType:levels:slices:",  (void *)&mtl_tex_newViewRange);
    oh2_objc_register_instance_method("MTLTexture", "replaceRegion:mipmapLevel:withBytes:bytesPerRow:",          (void *)&mtl_tex_replaceRegion2D);
    oh2_objc_register_instance_method("MTLTexture", "replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:", (void *)&mtl_tex_replaceRegionSlice);

    oh2_objc_register_instance_method("MTLSamplerDescriptor", "init",                  (void *)&mtl_sampdesc_init);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "copy",                  (void *)&mtl_sampdesc_copy);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "copyWithZone:",         (void *)&mtl_sampdesc_copyWithZone);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "minFilter",             (void *)&mtl_sd_get_min);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setMinFilter:",         (void *)&mtl_sd_set_min);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "magFilter",             (void *)&mtl_sd_get_mag);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setMagFilter:",         (void *)&mtl_sd_set_mag);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "mipFilter",             (void *)&mtl_sd_get_mip);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setMipFilter:",         (void *)&mtl_sd_set_mip);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "sAddressMode",          (void *)&mtl_sd_get_sa);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setSAddressMode:",      (void *)&mtl_sd_set_sa);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "tAddressMode",          (void *)&mtl_sd_get_ta);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setTAddressMode:",      (void *)&mtl_sd_set_ta);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "rAddressMode",          (void *)&mtl_sd_get_ra);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setRAddressMode:",      (void *)&mtl_sd_set_ra);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "maxAnisotropy",         (void *)&mtl_sd_get_aniso);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setMaxAnisotropy:",     (void *)&mtl_sd_set_aniso);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "lodMinClamp",           (void *)&mtl_sd_get_lmin);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setLodMinClamp:",       (void *)&mtl_sd_set_lmin);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "lodMaxClamp",           (void *)&mtl_sd_get_lmax);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setLodMaxClamp:",       (void *)&mtl_sd_set_lmax);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "compareFunction",       (void *)&mtl_sd_get_cmp);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setCompareFunction:",   (void *)&mtl_sd_set_cmp);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "borderColor",           (void *)&mtl_sd_get_border);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setBorderColor:",       (void *)&mtl_sd_set_border);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "normalizedCoordinates", (void *)&mtl_sd_get_norm);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setNormalizedCoordinates:", (void *)&mtl_sd_set_norm);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "lodAverage",            (void *)&mtl_sd_false);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setLodAverage:",        (void *)&mtl_sd_noop_u8);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "supportArgumentBuffers", (void *)&mtl_sd_false);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setSupportArgumentBuffers:", (void *)&mtl_sd_noop_u8);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "label",                 (void *)&mtl_sd_label);
    oh2_objc_register_instance_method("MTLSamplerDescriptor", "setLabel:",             (void *)&mtl_buffer_setLabel);

    oh2_objc_register_instance_method("MTLSamplerState", "device",                     (void *)&mtl_buffer_device);
    oh2_objc_register_instance_method("MTLSamplerState", "label",                      (void *)&mtl_sd_label);

    oh2_objc_register_instance_method("MTLStencilDescriptor", "init",                        (void *)&mtl_stencildesc_init);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "copy",                        (void *)&mtl_stencildesc_copy);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "copyWithZone:",               (void *)&mtl_stencildesc_copyWithZone);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "setStencilCompareFunction:",  (void *)&mtl_st_cmp);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "setStencilFailureOperation:", (void *)&mtl_st_sfail);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "setDepthFailureOperation:",   (void *)&mtl_st_dfail);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "setDepthStencilPassOperation:", (void *)&mtl_st_pass);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "setReadMask:",                (void *)&mtl_st_rmask);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "setWriteMask:",               (void *)&mtl_st_wmask);

    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "init",                   (void *)&mtl_dsdesc_init);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "copy",                   (void *)&mtl_dsdesc_copy);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "copyWithZone:",          (void *)&mtl_dsdesc_copyWithZone);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "label",                  (void *)&mtl_sd_label);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "setLabel:",              (void *)&mtl_buffer_setLabel);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "depthCompareFunction",   (void *)&mtl_ds_get_cmp);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "setDepthCompareFunction:", (void *)&mtl_ds_set_cmp);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "isDepthWriteEnabled",    (void *)&mtl_ds_get_wr);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "setDepthWriteEnabled:",  (void *)&mtl_ds_set_wr);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "setFrontFaceStencil:",   (void *)&mtl_ds_set_front);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "setBackFaceStencil:",    (void *)&mtl_ds_set_back);
    static auto mtl_ds_get_front = +[](void *s, const char *sel) -> void * {
        (void)sel;
        auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s);
        if (!d) return nullptr;
        if (!d->front_wrap) d->front_wrap = alloc_mtl_wrap_managed("MTLStencilDescriptor", &d->front, &mtl_wrap_only_noop_dealloc, false);
        return d->front_wrap;
    };
    static auto mtl_ds_get_back = +[](void *s, const char *sel) -> void * {
        (void)sel;
        auto *d = unwrap_mtl<MtlDepthStencilDescriptorImpl>(s);
        if (!d) return nullptr;
        if (!d->back_wrap) d->back_wrap = alloc_mtl_wrap_managed("MTLStencilDescriptor", &d->back, &mtl_wrap_only_noop_dealloc, false);
        return d->back_wrap;
    };
    static auto mtl_st_get_cmp   = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s); return d ? d->compare_fn : 7; };
    static auto mtl_st_get_sfail = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s); return d ? d->stencil_fail_op : 0; };
    static auto mtl_st_get_dfail = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s); return d ? d->depth_fail_op : 0; };
    static auto mtl_st_get_pass  = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s); return d ? d->depth_stencil_pass_op : 0; };
    static auto mtl_st_get_rmask = +[](void *s, const char *sel) -> uint32_t { (void)sel; auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s); return d ? d->read_mask : 0xFFFFFFFFu; };
    static auto mtl_st_get_wmask = +[](void *s, const char *sel) -> uint32_t { (void)sel; auto *d = unwrap_mtl<MtlStencilDescriptorImpl>(s); return d ? d->write_mask : 0xFFFFFFFFu; };
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "frontFaceStencil",         (void *)mtl_ds_get_front);
    oh2_objc_register_instance_method("MTLDepthStencilDescriptor", "backFaceStencil",          (void *)mtl_ds_get_back);

    oh2_objc_ensure_class("MTLDepthStencilState", "NSObject");
    oh2_objc_register_instance_method("MTLDepthStencilState", "device",                        (void *)&mtl_buffer_device);
    oh2_objc_register_instance_method("MTLDepthStencilState", "label",                         (void *)&mtl_sd_label);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "stencilCompareFunction",        (void *)mtl_st_get_cmp);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "stencilFailureOperation",       (void *)mtl_st_get_sfail);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "depthFailureOperation",         (void *)mtl_st_get_dfail);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "depthStencilPassOperation",     (void *)mtl_st_get_pass);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "readMask",                      (void *)mtl_st_get_rmask);
    oh2_objc_register_instance_method("MTLStencilDescriptor", "writeMask",                     (void *)mtl_st_get_wmask);

    oh2_objc_register_instance_method("CAMetalLayer", "init",                       (void *)&mtl_layer_init);
    oh2_objc_register_instance_method("CAMetalLayer", "device",                     (void *)&mtl_layer_get_dev);
    oh2_objc_register_instance_method("CAMetalLayer", "setDevice:",                 (void *)&mtl_layer_set_dev);
    oh2_objc_register_instance_method("CAMetalLayer", "pixelFormat",                (void *)&mtl_layer_get_fmt);
    oh2_objc_register_instance_method("CAMetalLayer", "setPixelFormat:",            (void *)&mtl_layer_set_fmt);
    oh2_objc_register_instance_method("CAMetalLayer", "isFramebufferOnly",          (void *)&mtl_layer_get_fbo);
    oh2_objc_register_instance_method("CAMetalLayer", "framebufferOnly",            (void *)&mtl_layer_get_fbo);
    oh2_objc_register_instance_method("CAMetalLayer", "setFramebufferOnly:",        (void *)&mtl_layer_set_fbo);
    oh2_objc_register_instance_method("CAMetalLayer", "drawableSize",               (void *)&mtl_layer_get_size);
    oh2_objc_register_instance_method("CAMetalLayer", "setDrawableSize:",           (void *)&mtl_layer_set_size);
    oh2_objc_register_instance_method("CAMetalLayer", "setContentsScale:",          (void *)&mtl_layer_set_scale);
    oh2_objc_register_instance_method("CAMetalLayer", "setPresentsWithTransaction:", (void *)&mtl_layer_set_tx);
    oh2_objc_register_instance_method("CAMetalLayer", "setDrawsAsynchronously:",    (void *)&mtl_layer_noop_u8);
    oh2_objc_register_instance_method("CAMetalLayer", "setOpaque:",                 (void *)&mtl_layer_noop_u8);
    oh2_objc_register_instance_method("CAMetalLayer", "setBackgroundColor:",        (void *)&mtl_layer_noop_ptr);
    oh2_objc_register_instance_method("CAMetalLayer", "setDrawableProperties:",     (void *)&mtl_layer_noop_ptr);
    oh2_objc_register_instance_method("CAMetalLayer", "bounds",                     (void *)&mtl_layer_get_bounds);
    oh2_objc_register_instance_method("CAMetalLayer", "setBounds:",                 (void *)&mtl_layer_set_bounds);
    oh2_objc_register_instance_method("CAMetalLayer", "frame",                      (void *)&mtl_layer_get_bounds);
    oh2_objc_register_instance_method("CAMetalLayer", "setFrame:",                  (void *)&mtl_layer_set_bounds);
    oh2_objc_register_instance_method("CAMetalLayer", "magnificationFilter",        (void *)&mtl_tex_label);
    oh2_objc_register_instance_method("CAMetalLayer", "setMagnificationFilter:",    (void *)&mtl_layer_noop_ptr);
    oh2_objc_register_instance_method("CAMetalLayer", "nextDrawable",               (void *)&mtl_layer_nextDrawable);

    static auto mtl_drawable_presTime  = +[](void *s, const char *sel, double t) { mtl_drawable_present(s, sel); (void)t; };
    static auto mtl_drawable_addPres   = +[](void *s, const char *sel, void *blk) {
        (void)sel;
        if (blk) {
            struct Blk1 { void *isa; int32_t f, r; void (*inv)(void *, void *); };
            auto *b = (Blk1 *)blk;
            if (b->inv) b->inv(blk, s);
        }
    };
    static auto mtl_drawable_time      = +[](void *s, const char *sel) -> double { (void)s; (void)sel; return 0.0; };
    oh2_objc_register_instance_method("CAMetalDrawable", "texture",                 (void *)&mtl_drawable_texture);
    oh2_objc_register_instance_method("CAMetalDrawable", "drawableID",              (void *)&mtl_drawable_id);
    oh2_objc_register_instance_method("CAMetalDrawable", "layer",                   (void *)&mtl_drawable_layer);
    oh2_objc_register_instance_method("CAMetalDrawable", "present",                 (void *)&mtl_drawable_present);
    oh2_objc_register_instance_method("CAMetalDrawable", "presentAtTime:",          (void *)mtl_drawable_presTime);
    oh2_objc_register_instance_method("CAMetalDrawable", "presentAfterMinimumDuration:", (void *)mtl_drawable_presTime);
    oh2_objc_register_instance_method("CAMetalDrawable", "addPresentedHandler:",    (void *)mtl_drawable_addPres);
    oh2_objc_register_instance_method("CAMetalDrawable", "presentedTime",           (void *)mtl_drawable_time);
}

void mtl_sync_buffer_backed_texture_if_dirty(MtlTextureImpl *tex) {
    if (!tex || !tex->parent_buffer_obj || tex->image == VK_NULL_HANDLE) return;
    auto *buf = unwrap_mtl<MtlBufferImpl>(tex->parent_buffer_obj);
    if (!buf || !buf->mapped) return;
    uint64_t cur_gen = buf->mod_gen.load(std::memory_order_relaxed);
    if (tex->synced_buf_gen == cur_gen) return;
    uint64_t cur_hash = hash_buffer_tex_slice(tex, buf);
    if (cur_hash != tex->synced_content_hash) {
        upload_buffer_backed_texture_cpu(tex, buf);
    } else {
        tex->synced_buf_gen = cur_gen;
    }
}

void mtl_sync_all_dirty_buffer_textures_cmd(MtlCommandBufferImpl *cb) {
    MtlVkState &st = get_state();
    if (!cb || !cb->recording || cb->cmd == VK_NULL_HANDLE || !st.initialized) return;
    if (!st.any_buf_tex_dirty.exchange(false, std::memory_order_acq_rel)) return;

    std::vector<MtlTextureImpl *> snapshot;
    {
        std::lock_guard<std::mutex> blk(st.buf_tex_mutex);
        if (st.active_buf_textures.empty()) return;
        snapshot = st.active_buf_textures;
    }

    struct PendingBufTexCopy {
        MtlTextureImpl   *tex;
        VkBuffer          src_buf;
        VkBufferImageCopy bic;
        VkImageLayout     old_l;
        uint64_t          cur_gen;
        uint64_t          cur_hash;
    };
    std::vector<PendingBufTexCopy> copies;
    copies.reserve(snapshot.size());
    std::vector<uint8_t> packed_tmp;

    for (MtlTextureImpl *tex : snapshot) {
        if (!tex || !tex->parent_buffer_obj || tex->image == VK_NULL_HANDLE) continue;
        auto *buf = unwrap_mtl<MtlBufferImpl>(tex->parent_buffer_obj);
        if (!buf || !buf->mapped || tex->buffer_offset >= buf->length) continue;
        uint64_t cur_gen = buf->mod_gen.load(std::memory_order_relaxed);
        if (tex->synced_buf_gen == cur_gen) continue;

        uint64_t cur_hash = hash_buffer_tex_slice(tex, buf);
        if (tex->synced_buf_gen != 0 && cur_hash == tex->synced_content_hash) {
            tex->synced_buf_gen = cur_gen;
            continue;
        }

        uint32_t w = std::max(1u, tex->width);
        uint32_t h = std::max(1u, tex->height);
        uint32_t d = std::max(1u, tex->depth);
        uint32_t bw = 1, bh = 1;
        uint32_t bsz = mtl_pixel_format_block_size(tex->vk_format, &bw, &bh);
        uint32_t bx = (w + bw - 1u) / bw;
        uint32_t by = (h + bh - 1u) / bh;
        uint64_t packed_row = static_cast<uint64_t>(bx) * bsz;
        uint64_t src_row_pitch = (tex->buffer_bytes_per_row > 0) ? tex->buffer_bytes_per_row : packed_row;
        uint64_t total_packed = packed_row * by * d;
        if (total_packed == 0 || total_packed > 4ULL * 1024ULL * 1024ULL) continue;

        uint64_t avail = buf->length - tex->buffer_offset;
        uint64_t total_span = (by * d > 1) ? ((uint64_t)(by * d - 1) * src_row_pitch + packed_row) : packed_row;
        uint64_t align_req = std::max<uint64_t>(4ULL, bsz);

        VkBuffer src_buf = VK_NULL_HANDLE;
        VkBufferImageCopy bic{};
        bic.imageSubresource = {tex->aspect_mask, 0, 0, 1};
        bic.imageOffset = {0, 0, 0};
        bic.imageExtent = {w, h, d};

        if (buf->buffer != VK_NULL_HANDLE &&
            (tex->buffer_offset % align_req) == 0 &&
            (src_row_pitch % bsz) == 0 &&
            avail >= total_span) {
            uint32_t row_texels = (src_row_pitch == packed_row) ? 0u : static_cast<uint32_t>((src_row_pitch / bsz) * bw);
            if (row_texels == 0 || (row_texels >= w && row_texels <= 65536u)) {
                if (buf->allocation) {
                    vmaFlushAllocation(st.vk_ctx.allocator(), buf->allocation, tex->buffer_offset, total_span);
                }
                src_buf = buf->buffer;
                bic.bufferOffset = tex->buffer_offset;
                bic.bufferRowLength = (h > 1) ? row_texels : 0u;
            }
        }

        if (src_buf == VK_NULL_HANDLE) {
            packed_tmp.assign(static_cast<size_t>(total_packed), 0);
            const uint8_t *src_base = static_cast<const uint8_t *>(buf->mapped) + tex->buffer_offset;
            if (src_row_pitch == packed_row && avail >= total_packed) {
                std::memcpy(packed_tmp.data(), src_base, static_cast<size_t>(total_packed));
            } else {
                for (uint32_t row = 0; row < by * d; ++row) {
                    uint64_t src_off = static_cast<uint64_t>(row) * src_row_pitch;
                    if (src_off >= avail) break;
                    uint64_t can_copy = std::min<uint64_t>(packed_row, avail - src_off);
                    std::memcpy(packed_tmp.data() + static_cast<uint64_t>(row) * packed_row,
                                src_base + src_off, static_cast<size_t>(can_copy));
                }
            }
            uint64_t inline_off = 0;
            if (!mtl_alloc_inline_bytes(cb, packed_tmp.data(), total_packed, &src_buf, &inline_off)) {
                continue;
            }
            bic.bufferOffset = inline_off;
            bic.bufferRowLength = 0;
        }

        VkImageLayout old_l = tex->mip_layouts.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : tex->mip_layouts[0];
        copies.push_back({tex, src_buf, bic, old_l, cur_gen, cur_hash});
    }

    if (copies.empty()) return;

    /* Batch all PRE-copy transitions into 1 vkCmdPipelineBarrier, record all copies, then 1 POST-copy barrier */
    std::vector<VkImageMemoryBarrier> pre_b;
    std::vector<VkImageMemoryBarrier> post_b;
    pre_b.reserve(copies.size());
    post_b.reserve(copies.size());
    VkPipelineStageFlags pre_src_stages = 0;

    for (const auto &c : copies) {
        if (c.old_l != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = c.old_l;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = c.tex->image;
            b.subresourceRange = {c.tex->aspect_mask, 0, 1, 0, 1};
            if (c.old_l == VK_IMAGE_LAYOUT_UNDEFINED) {
                b.srcAccessMask = 0;
                pre_src_stages |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            } else if (c.old_l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                pre_src_stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            } else {
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
                pre_src_stages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            }
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pre_b.push_back(b);
        }
        VkImageMemoryBarrier pb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pb.image = c.tex->image;
        pb.subresourceRange = {c.tex->aspect_mask, 0, 1, 0, 1};
        pb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        post_b.push_back(pb);
    }

    if (!pre_b.empty()) {
        vkCmdPipelineBarrier(cb->cmd,
                             pre_src_stages ? pre_src_stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(pre_b.size()), pre_b.data());
    }
    for (const auto &c : copies) {
        vkCmdCopyBufferToImage(cb->cmd, c.src_buf, c.tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c.bic);
        if (!c.tex->mip_layouts.empty()) c.tex->mip_layouts[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        c.tex->synced_buf_gen = c.cur_gen;
        c.tex->synced_content_hash = c.cur_hash;
    }
    vkCmdPipelineBarrier(cb->cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(post_b.size()), post_b.data());
    st.prof_buftex_uploads.fetch_add(copies.size(), std::memory_order_relaxed);
}

void mtl_queue_btex_for_preamble(MtlCommandBufferImpl *cb, MtlTextureImpl *tex) {
    MtlVkState &st = get_state();
    if (!cb || !tex || !tex->parent_buffer_obj || tex->image == VK_NULL_HANDLE || !st.initialized) return;
    if (!cb->recording && mtl_begin_command_buffer_internal(cb) == VK_NULL_HANDLE) return;
    auto *buf = unwrap_mtl<MtlBufferImpl>(tex->parent_buffer_obj);
    if (!buf || !buf->mapped || tex->buffer_offset >= buf->length) return;
    uint64_t cur_gen = buf->mod_gen.load(std::memory_order_relaxed);
    if (tex->synced_buf_gen == cur_gen || tex->synced_cmd_seq == cb->cmd_seq_id) return;
    tex->synced_cmd_seq = cb->cmd_seq_id;
    st.frames[cb->frame_idx].preamble_btex_list.push_back(tex);
}

bool mtl_record_preamble_btex_copies(MtlCommandBufferImpl *cb) {
    MtlVkState &st = get_state();
    if (!cb || !st.initialized) return false;
    FrameSyncSlot &slot = st.frames[cb->frame_idx];
    if (slot.preamble_btex_list.empty() || slot.preamble_cmd == VK_NULL_HANDLE) {
        slot.preamble_btex_list.clear();
        return false;
    }

    struct PendingBufTexCopy {
        MtlTextureImpl   *tex;
        VkBuffer          src_buf;
        VkBufferImageCopy bic;
        VkImageLayout     old_l;
        uint64_t          cur_gen;
    };
    std::vector<PendingBufTexCopy> copies;
    copies.reserve(slot.preamble_btex_list.size());
    std::vector<uint8_t> packed_tmp;

    for (MtlTextureImpl *tex : slot.preamble_btex_list) {
        if (!tex || !tex->parent_buffer_obj || tex->image == VK_NULL_HANDLE) continue;
        auto *buf = unwrap_mtl<MtlBufferImpl>(tex->parent_buffer_obj);
        if (!buf || !buf->mapped || tex->buffer_offset >= buf->length) continue;
        uint64_t cur_gen = buf->mod_gen.load(std::memory_order_relaxed);
        if (tex->synced_buf_gen == cur_gen) continue;

        uint32_t w = std::max(1u, tex->width);
        uint32_t h = std::max(1u, tex->height);
        uint32_t d = std::max(1u, tex->depth);
        uint32_t bw = 1, bh = 1;
        uint32_t bsz = mtl_pixel_format_block_size(tex->vk_format, &bw, &bh);
        uint32_t bx = (w + bw - 1u) / bw;
        uint32_t by = (h + bh - 1u) / bh;
        uint64_t packed_row = static_cast<uint64_t>(bx) * bsz;
        uint64_t src_row_pitch = (tex->buffer_bytes_per_row > 0) ? tex->buffer_bytes_per_row : packed_row;
        uint64_t total_packed = packed_row * by * d;
        if (total_packed == 0 || total_packed > 4ULL * 1024ULL * 1024ULL) continue;

        uint64_t avail = buf->length - tex->buffer_offset;
        uint64_t total_span = (by * d > 1) ? ((uint64_t)(by * d - 1) * src_row_pitch + packed_row) : packed_row;
        uint64_t align_req = std::max<uint64_t>(4ULL, bsz);

        VkBuffer src_buf = VK_NULL_HANDLE;
        VkBufferImageCopy bic{};
        bic.imageSubresource = {tex->aspect_mask, 0, 0, 1};
        bic.imageOffset = {0, 0, 0};
        bic.imageExtent = {w, h, d};

        if (buf->buffer != VK_NULL_HANDLE &&
            (tex->buffer_offset % align_req) == 0 &&
            (src_row_pitch % bsz) == 0 &&
            avail >= total_span) {
            uint32_t row_texels = (src_row_pitch == packed_row) ? 0u : static_cast<uint32_t>((src_row_pitch / bsz) * bw);
            if (row_texels == 0 || (row_texels >= w && row_texels <= 65536u)) {
                if (buf->allocation) {
                    vmaFlushAllocation(st.vk_ctx.allocator(), buf->allocation, tex->buffer_offset, total_span);
                }
                src_buf = buf->buffer;
                bic.bufferOffset = tex->buffer_offset;
                bic.bufferRowLength = (h > 1) ? row_texels : 0u;
            }
        }

        if (src_buf == VK_NULL_HANDLE) {
            packed_tmp.assign(static_cast<size_t>(total_packed), 0);
            const uint8_t *src_base = static_cast<const uint8_t *>(buf->mapped) + tex->buffer_offset;
            if (src_row_pitch == packed_row && avail >= total_packed) {
                std::memcpy(packed_tmp.data(), src_base, static_cast<size_t>(total_packed));
            } else {
                for (uint32_t row = 0; row < by * d; ++row) {
                    uint64_t src_off = static_cast<uint64_t>(row) * src_row_pitch;
                    if (src_off >= avail) break;
                    uint64_t can_copy = std::min<uint64_t>(packed_row, avail - src_off);
                    std::memcpy(packed_tmp.data() + static_cast<uint64_t>(row) * packed_row,
                                src_base + src_off, static_cast<size_t>(can_copy));
                }
            }
            uint64_t inline_off = 0;
            if (!mtl_alloc_inline_bytes(cb, packed_tmp.data(), total_packed, &src_buf, &inline_off)) {
                continue;
            }
            bic.bufferOffset = inline_off;
            bic.bufferRowLength = 0;
        }

        VkImageLayout old_l = tex->mip_layouts.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : tex->mip_layouts[0];
        copies.push_back({tex, src_buf, bic, old_l, cur_gen});
    }
    slot.preamble_btex_list.clear();
    if (copies.empty()) return false;

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.preamble_cmd, &begin_info) != VK_SUCCESS) return false;

    std::vector<VkImageMemoryBarrier> pre_b;
    std::vector<VkImageMemoryBarrier> post_b;
    pre_b.reserve(copies.size());
    post_b.reserve(copies.size());
    VkPipelineStageFlags pre_src_stages = 0;

    for (const auto &c : copies) {
        if (c.old_l != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = c.old_l;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = c.tex->image;
            b.subresourceRange = {c.tex->aspect_mask, 0, 1, 0, 1};
            if (c.old_l == VK_IMAGE_LAYOUT_UNDEFINED) {
                b.srcAccessMask = 0;
                pre_src_stages |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            } else if (c.old_l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                pre_src_stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            } else {
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
                pre_src_stages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            }
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pre_b.push_back(b);
        }
        VkImageMemoryBarrier pb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pb.image = c.tex->image;
        pb.subresourceRange = {c.tex->aspect_mask, 0, 1, 0, 1};
        pb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        post_b.push_back(pb);
    }

    if (!pre_b.empty()) {
        vkCmdPipelineBarrier(slot.preamble_cmd,
                             pre_src_stages ? pre_src_stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(pre_b.size()), pre_b.data());
    }
    for (const auto &c : copies) {
        vkCmdCopyBufferToImage(slot.preamble_cmd, c.src_buf, c.tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c.bic);
        if (!c.tex->mip_layouts.empty()) c.tex->mip_layouts[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        c.tex->synced_buf_gen = c.cur_gen;
    }
    vkCmdPipelineBarrier(slot.preamble_cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(post_b.size()), post_b.data());

    if (vkEndCommandBuffer(slot.preamble_cmd) != VK_SUCCESS) return false;
    st.prof_buftex_uploads.fetch_add(copies.size(), std::memory_order_relaxed);
    return true;
}

void mtl_flush_pending_tex_uploads_cmd(MtlCommandBufferImpl *cb) {
    MtlVkState &st = get_state();
    if (!cb || !cb->recording || cb->cmd == VK_NULL_HANDLE || !st.initialized) return;
    if (!st.has_pending_tex_uploads.exchange(false, std::memory_order_acq_rel)) return;

    std::vector<PendingTexUpload> batch;
    {
        std::lock_guard<std::mutex> plk(st.pending_tex_mutex);
        batch.swap(st.pending_tex_uploads);
    }
    if (batch.empty()) return;

    FrameSyncSlot &slot = st.frames[cb->frame_idx];
    for (auto &item : batch) {
        auto *tex = unwrap_mtl<MtlTextureImpl>(item.tex_obj);
        if (tex && tex->image != VK_NULL_HANDLE && item.level < tex->mip_levels &&
            item.slice < tex->array_layers && !item.bytes.empty()) {
            VkBuffer src_buf = VK_NULL_HANDLE;
            uint64_t src_off = 0;
            bool ok = false;
            if (item.bytes.size() <= 2ULL * 1024ULL * 1024ULL) {
                ok = mtl_alloc_inline_bytes(cb, item.bytes.data(), item.bytes.size(), &src_buf, &src_off);
            }
            if (!ok) {
                oh2::vk::StagingAllocation staging{};
                if (st.staging.allocate(item.bytes.size(),
                                        std::max<VkDeviceSize>(16, st.vk_ctx.caps().optimal_buffer_copy_offset_alignment),
                                        staging)) {
                    std::memcpy(staging.mapped, item.bytes.data(), item.bytes.size());
                    st.staging.flush(staging.offset, item.bytes.size());
                    src_buf = staging.buffer;
                    src_off = staging.offset;
                    if (staging.end_cursor > slot.staging_end_cursor) {
                        slot.staging_end_cursor = staging.end_cursor;
                    }
                    ok = true;
                }
            }
            if (ok) {
                VkImageLayout old_l = tex->get_subres_layout(item.level, item.slice);
                transition_image_layout(cb->cmd, tex->image, tex->aspect_mask,
                                        item.level, 1, item.slice, 1,
                                        old_l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy bic{};
                bic.bufferOffset = src_off;
                bic.bufferRowLength = 0;
                bic.bufferImageHeight = 0;
                bic.imageSubresource.aspectMask = tex->aspect_mask;
                bic.imageSubresource.mipLevel = item.level;
                bic.imageSubresource.baseArrayLayer = item.slice;
                bic.imageSubresource.layerCount = 1;
                bic.imageOffset = {(int32_t)item.reg.origin.x, (int32_t)item.reg.origin.y, (int32_t)item.reg.origin.z};
                bic.imageExtent = {(uint32_t)std::max<uint64_t>(1, item.reg.size.width),
                                   (uint32_t)std::max<uint64_t>(1, item.reg.size.height),
                                   (uint32_t)std::max<uint64_t>(1, item.reg.size.depth)};
                vkCmdCopyBufferToImage(cb->cmd, src_buf, tex->image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
                transition_image_layout(cb->cmd, tex->image, tex->aspect_mask,
                                        item.level, 1, item.slice, 1,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                tex->set_subres_layout(item.level, item.slice, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
        if (item.tex_obj) oh2_objc_release_object(item.tex_obj);
    }
}

} // namespace oh2::mtlvk
