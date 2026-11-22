#include "mtl_vk_internal.h"

#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
void oh2_objc_msgSend(void);
}

namespace oh2::mtlvk {

namespace {

/* Validated Vulkan 1.1 SPIR-V modules (verified by spirv-val --target-env vulkan1.1) */
static const uint32_t kTestVsWords[] = {
    0x07230203u, 0x00010300u, 0x000d000bu, 0x0000001fu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0009000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000012u, 0x0000001bu,
    0x0000001du, 0x00030047u, 0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu,
    0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu,
    0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u,
    0x00040047u, 0x00000012u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000001bu, 0x0000001eu, 0x00000000u,
    0x00040047u, 0x0000001du, 0x0000001eu, 0x00000001u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
    0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u,
    0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000008u, 0x00000009u, 0x00000001u,
    0x0004001cu, 0x0000000au, 0x00000006u, 0x00000009u, 0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u,
    0x0000000au, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu, 0x0004003bu, 0x0000000cu,
    0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu,
    0x0000000fu, 0x00000000u, 0x00040017u, 0x00000010u, 0x00000006u, 0x00000002u, 0x00040020u, 0x00000011u,
    0x00000001u, 0x00000010u, 0x0004003bu, 0x00000011u, 0x00000012u, 0x00000001u, 0x0004002bu, 0x00000006u,
    0x00000014u, 0x00000000u, 0x0004002bu, 0x00000006u, 0x00000015u, 0x3f800000u, 0x00040020u, 0x00000019u,
    0x00000003u, 0x00000007u, 0x0004003bu, 0x00000019u, 0x0000001bu, 0x00000003u, 0x00040020u, 0x0000001cu,
    0x00000001u, 0x00000007u, 0x0004003bu, 0x0000001cu, 0x0000001du, 0x00000001u, 0x00050036u, 0x00000002u,
    0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x00000010u, 0x00000013u,
    0x00000012u, 0x00050051u, 0x00000006u, 0x00000016u, 0x00000013u, 0x00000000u, 0x00050051u, 0x00000006u,
    0x00000017u, 0x00000013u, 0x00000001u, 0x00070050u, 0x00000007u, 0x00000018u, 0x00000016u, 0x00000017u,
    0x00000014u, 0x00000015u, 0x00050041u, 0x00000019u, 0x0000001au, 0x0000000du, 0x0000000fu, 0x0003003eu,
    0x0000001au, 0x00000018u, 0x0004003du, 0x00000007u, 0x0000001eu, 0x0000001du, 0x0003003eu, 0x0000001bu,
    0x0000001eu, 0x000100fdu, 0x00010038u,
};

static const uint32_t kTestPsWords[] = {
    0x07230203u, 0x00010300u, 0x000d000bu, 0x00000016u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x0000000bu, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000bu,
    0x0000001eu, 0x00000000u, 0x00030047u, 0x0000000du, 0x00000002u, 0x00050048u, 0x0000000du, 0x00000000u,
    0x00000023u, 0x00000000u, 0x00040047u, 0x0000000fu, 0x00000021u, 0x00000000u, 0x00040047u, 0x0000000fu,
    0x00000022u, 0x00000001u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u,
    0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u,
    0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x00040020u, 0x0000000au,
    0x00000001u, 0x00000007u, 0x0004003bu, 0x0000000au, 0x0000000bu, 0x00000001u, 0x0003001eu, 0x0000000du,
    0x00000007u, 0x00040020u, 0x0000000eu, 0x00000002u, 0x0000000du, 0x0004003bu, 0x0000000eu, 0x0000000fu,
    0x00000002u, 0x00040015u, 0x00000010u, 0x00000020u, 0x00000001u, 0x0004002bu, 0x00000010u, 0x00000011u,
    0x00000000u, 0x00040020u, 0x00000012u, 0x00000002u, 0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u,
    0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u, 0x0000000cu, 0x0000000bu,
    0x00050041u, 0x00000012u, 0x00000013u, 0x0000000fu, 0x00000011u, 0x0004003du, 0x00000007u, 0x00000014u,
    0x00000013u, 0x00050085u, 0x00000007u, 0x00000015u, 0x0000000cu, 0x00000014u, 0x0003003eu, 0x00000009u,
    0x00000015u, 0x000100fdu, 0x00010038u,
};

struct TestVertex {
    float x, y;
    float r, g, b, a;
};

static const SpirvVertexAttrDesc kTestVsAttrs[2] = {
    {0, SPIRV_COMP_FLOAT, 2, 0, 0},
    {1, SPIRV_COMP_FLOAT, 4, 0, 0},
};
static const SpirvBindingDesc kTestPsBinding = {
    1, 0, SPIRV_VK_DESC_UNIFORM_BUFFER, 0, SPIRV_METAL_KIND_BUFFER, 0, 0, 1, 16
};

static void *msg0(void *o, const char *s) {
    return ((void *(*)(void *, const char *))oh2_objc_msgSend)(o, oh2_objc_intern_selector(s));
}
static void *msg1(void *o, const char *s, void *a) {
    return ((void *(*)(void *, const char *, void *))oh2_objc_msgSend)(o, oh2_objc_intern_selector(s), a);
}
static void msg1v(void *o, const char *s, void *a) {
    ((void (*)(void *, const char *, void *))oh2_objc_msgSend)(o, oh2_objc_intern_selector(s), a);
}
static void *create_test_quad_pso(void *dev, uint64_t color_fmt) {
    auto *vs_entry = new SpirvShaderEntry{};
    vs_entry->shader_id = 0x5445535456533031ULL;
    vs_entry->stage = SPIRV_STAGE_VERTEX;
    vs_entry->spirv_words = kTestVsWords;
    vs_entry->spirv_size_bytes = sizeof(kTestVsWords);
    vs_entry->spirv_word_count = sizeof(kTestVsWords) / 4;
    vs_entry->entry_name = "TestVS";
    vs_entry->spirv_entry_point = "main";
    vs_entry->vertex_attrs = kTestVsAttrs;
    vs_entry->vertex_attr_count = 2;

    auto *ps_entry = new SpirvShaderEntry{};
    ps_entry->shader_id = 0x5445535450533031ULL;
    ps_entry->stage = SPIRV_STAGE_FRAGMENT;
    ps_entry->spirv_words = kTestPsWords;
    ps_entry->spirv_size_bytes = sizeof(kTestPsWords);
    ps_entry->spirv_word_count = sizeof(kTestPsWords) / 4;
    ps_entry->entry_name = "TestPS";
    ps_entry->spirv_entry_point = "main";
    ps_entry->bindings = &kTestPsBinding;
    ps_entry->binding_count = 1;
    ps_entry->buffer_mask = 1;

    auto *vs_fn = new MtlFunctionImpl{vs_entry, create_shader_module_from_words(kTestVsWords, sizeof(kTestVsWords)), 1, "TestVS"};
    auto *ps_fn = new MtlFunctionImpl{ps_entry, create_shader_module_from_words(kTestPsWords, sizeof(kTestPsWords)), 2, "TestPS"};
    void *vs_obj = alloc_mtl_wrap("MTLFunction", vs_fn);
    void *ps_obj = alloc_mtl_wrap("MTLFunction", ps_fn);

    void *rpl_cls = oh2_objc_get_class("MTLRenderPipelineDescriptor");
    void *rpl = msg0(msg0(rpl_cls, "alloc"), "init");
    msg1v(rpl, "setVertexFunction:", vs_obj);
    msg1v(rpl, "setFragmentFunction:", ps_obj);

    void *vd = msg0(oh2_objc_get_class("MTLVertexDescriptor"), "vertexDescriptor");
    void *attrs = msg0(vd, "attributes");
    void *layouts = msg0(vd, "layouts");

    typedef void *(*sub_t)(void *, const char *, uint64_t);
    void *a0 = ((sub_t)oh2_objc_msgSend)(attrs, "objectAtIndexedSubscript:", 0);
    void *a1 = ((sub_t)oh2_objc_msgSend)(attrs, "objectAtIndexedSubscript:", 1);
    void *l0 = ((sub_t)oh2_objc_msgSend)(layouts, "objectAtIndexedSubscript:", 0);

    typedef void (*setu64_t)(void *, const char *, uint64_t);
    ((setu64_t)oh2_objc_msgSend)(a0, "setFormat:", 29); /* Float2 */
    ((setu64_t)oh2_objc_msgSend)(a0, "setOffset:", 0);
    ((setu64_t)oh2_objc_msgSend)(a0, "setBufferIndex:", 0);
    ((setu64_t)oh2_objc_msgSend)(a1, "setFormat:", 31); /* Float4 */
    ((setu64_t)oh2_objc_msgSend)(a1, "setOffset:", 8);
    ((setu64_t)oh2_objc_msgSend)(a1, "setBufferIndex:", 0);
    ((setu64_t)oh2_objc_msgSend)(l0, "setStride:", sizeof(TestVertex));
    msg1v(rpl, "setVertexDescriptor:", vd);

    void *ca = msg0(rpl, "colorAttachments");
    void *c0 = ((sub_t)oh2_objc_msgSend)(ca, "objectAtIndexedSubscript:", 0);
    ((setu64_t)oh2_objc_msgSend)(c0, "setPixelFormat:", color_fmt);

    typedef void *(*new_pso_t)(void *, const char *, void *, void **);
    void *err = nullptr;
    return ((new_pso_t)oh2_objc_msgSend)(dev, "newRenderPipelineStateWithDescriptor:error:", rpl, &err);
}

static bool test_metal_objects() {
    void *(*create_dev)(void) = (void *(*)(void))darwin_resolve_symbol(nullptr, "_MTLCreateSystemDefaultDevice");
    void *dev = create_dev ? create_dev() : nullptr;
    void *q = msg0(dev, "newCommandQueue");
    if (!dev || !q) return false;

    /* MTLBuffer write/readback */
    const uint32_t pattern[4] = {0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u};
    typedef void *(*new_buf_t)(void *, const char *, const void *, uint64_t, uint64_t);
    void *buf = ((new_buf_t)oh2_objc_msgSend)(dev, "newBufferWithBytes:length:options:", pattern, sizeof(pattern), 0);
    void *mapped = msg0(buf, "contents");
    if (!buf || !mapped || std::memcmp(mapped, pattern, sizeof(pattern)) != 0) return false;

    /* ASTC 4x4 8x8 quad-color native upload (zero CPU RGBA8 decompression!) */
    size_t astc_sz = 0;
    const uint8_t *astc_bytes = oh2::vk::get_astc_4x4_8x8_quad_color_bytes(&astc_sz);
    typedef void *(*tex2d_t)(void *, const char *, uint64_t, uint64_t, uint64_t, uint8_t);
    void *td = ((tex2d_t)oh2_objc_msgSend)(oh2_objc_get_class("MTLTextureDescriptor"),
                                            "texture2DDescriptorWithPixelFormat:width:height:mipmapped:",
                                            204 /* ASTC_4x4_LDR */, 8, 8, 0);
    void *tex = msg1(dev, "newTextureWithDescriptor:", td);
    if (!tex) return false;
    MTLRegion reg{{0, 0, 0}, {8, 8, 1}};
    typedef void (*rep_t)(void *, const char *, MTLRegion *, uint64_t, const void *, uint64_t);
    ((rep_t)oh2_objc_msgSend)(tex, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", &reg, 0, astc_bytes, 32);

    /* MTLSamplerState & MTLDepthStencilState */
    void *sd = msg0(msg0(oh2_objc_get_class("MTLSamplerDescriptor"), "alloc"), "init");
    void *smp = msg1(dev, "newSamplerStateWithDescriptor:", sd);
    void *dsd = msg0(msg0(oh2_objc_get_class("MTLDepthStencilDescriptor"), "alloc"), "init");
    void *dss = msg1(dev, "newDepthStencilStateWithDescriptor:", dsd);
    if (!smp || !dss) return false;

    MtlVkStats stats{};
    mtl_vk_get_stats(&stats);
    return (stats.astc_textures_created >= 1 &&
            stats.astc_cpu_decompress_count == 0 &&
            stats.validation_errors == 0 &&
            stats.validation_warnings == 0);
}

static bool test_metal_render_pass() {
    if (spirv_cache_count() < 4480) {
        MTLVK_LOGE("metal_render_pass: expected >=4480 shaders in spirv_cache, got %u", spirv_cache_count());
        return false;
    }
    /* Verify creating VkShaderModule from real cached OH2 shaders */
    const SpirvShaderEntry *real_vs = nullptr;
    const SpirvShaderEntry *real_ps = nullptr;
    for (uint32_t i = 0; i < spirv_cache_count() && (!real_vs || !real_ps); ++i) {
        const SpirvShaderEntry *e = spirv_cache_entry_at(i);
        if (!e) continue;
        if (e->stage == SPIRV_STAGE_VERTEX && !real_vs) real_vs = e;
        if (e->stage == SPIRV_STAGE_FRAGMENT && !real_ps) real_ps = e;
    }
    if (!real_vs || !real_ps ||
        get_or_create_shader_module(real_vs) == VK_NULL_HANDLE ||
        get_or_create_shader_module(real_ps) == VK_NULL_HANDLE) {
        MTLVK_LOGE("metal_render_pass: failed creating cached VkShaderModule");
        return false;
    }

    void *(*create_dev)(void) = (void *(*)(void))darwin_resolve_symbol(nullptr, "_MTLCreateSystemDefaultDevice");
    void *dev = create_dev();
    void *q = msg0(dev, "newCommandQueue");
    void *layer = msg0(msg0(oh2_objc_get_class("CAMetalLayer"), "alloc"), "init");
    msg1v(layer, "setDevice:", dev);

    void *drawable = msg0(layer, "nextDrawable");
    if (!drawable) {
        MTLVK_LOGE("metal_render_pass: nextDrawable returned nullptr");
        return false;
    }
    void *tex = msg0(drawable, "texture");
    uint64_t fmt = (uint64_t)(uintptr_t)msg0(tex, "pixelFormat");
    void *pso = create_test_quad_pso(dev, fmt);
    if (!pso) {
        MTLVK_LOGE("metal_render_pass: create_test_quad_pso failed (fmt=%llu)", (unsigned long long)fmt);
        return false;
    }

    const TestVertex verts[3] = {
        { 0.0f,  0.6f, 0.2f, 0.8f, 0.3f, 1.0f},
        {-0.6f, -0.6f, 0.9f, 0.4f, 0.1f, 1.0f},
        { 0.6f, -0.6f, 0.1f, 0.5f, 0.9f, 1.0f},
    };
    typedef void *(*new_buf_t)(void *, const char *, const void *, uint64_t, uint64_t);
    void *vb = ((new_buf_t)oh2_objc_msgSend)(dev, "newBufferWithBytes:length:options:", verts, sizeof(verts), 0);

    void *rpd = msg0(oh2_objc_get_class("MTLRenderPassDescriptor"), "renderPassDescriptor");
    void *ca = msg0(rpd, "colorAttachments");
    void *c0 = ((void *(*)(void *, const char *, uint64_t))oh2_objc_msgSend)(ca, "objectAtIndexedSubscript:", 0);
    msg1v(c0, "setTexture:", tex);
    ((void (*)(void *, const char *, uint64_t))oh2_objc_msgSend)(c0, "setLoadAction:", kMTLLoadActionClear);
    ((void (*)(void *, const char *, uint64_t))oh2_objc_msgSend)(c0, "setStoreAction:", kMTLStoreActionStore);
    ((void (*)(void *, const char *, MTLClearColor))oh2_objc_msgSend)(c0, "setClearColor:", MTLClearColor{0.05, 0.12, 0.22, 1.0});

    void *cb = msg0(q, "commandBuffer");
    void *enc = msg1(cb, "renderCommandEncoderWithDescriptor:", rpd);
    msg1v(enc, "setRenderPipelineState:", pso);
    ((void (*)(void *, const char *, void *, uint64_t, uint64_t))oh2_objc_msgSend)(enc, "setVertexBuffer:offset:atIndex:", vb, 0, 0);
    const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    ((void (*)(void *, const char *, const void *, uint64_t, uint64_t))oh2_objc_msgSend)(enc, "setFragmentBytes:length:atIndex:", tint, sizeof(tint), 0);
    ((void (*)(void *, const char *, uint64_t, uint64_t, uint64_t))oh2_objc_msgSend)(enc, "drawPrimitives:vertexStart:vertexCount:", kMTLPrimitiveTriangle, 0, 3);
    msg0(enc, "endEncoding");
    msg1v(cb, "presentDrawable:", drawable);
    msg0(cb, "commit");
    msg0(cb, "waitUntilCompleted");

    MtlVkStats stats{};
    mtl_vk_get_stats(&stats);
    return (stats.presents >= 1 && stats.validation_errors == 0 && stats.validation_warnings == 0);
}

static bool test_encoder_scissor_load() {
    void *(*create_dev)(void) = (void *(*)(void))darwin_resolve_symbol(nullptr, "_MTLCreateSystemDefaultDevice");
    void *dev = create_dev();
    void *q = msg0(dev, "newCommandQueue");

    /* 64x64 offscreen RGBA8Unorm RT */
    void *td = ((void *(*)(void *, const char *, uint64_t, uint64_t, uint64_t, uint8_t))oh2_objc_msgSend)(
        oh2_objc_get_class("MTLTextureDescriptor"),
        "texture2DDescriptorWithPixelFormat:width:height:mipmapped:",
        70 /* RGBA8Unorm */, 64, 64, 0);
    ((void (*)(void *, const char *, uint64_t))oh2_objc_msgSend)(td, "setUsage:", 0x05 /* Read|RenderTarget */);
    void *rt = msg1(dev, "newTextureWithDescriptor:", td);
    void *pso = create_test_quad_pso(dev, 70);

    const TestVertex quad[4] = {
        {-1.0f, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        { 1.0f,  1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f,  1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    };
    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    typedef void *(*new_buf_t)(void *, const char *, const void *, uint64_t, uint64_t);
    void *vb = ((new_buf_t)oh2_objc_msgSend)(dev, "newBufferWithBytes:length:options:", quad, sizeof(quad), 0);
    void *ib = ((new_buf_t)oh2_objc_msgSend)(dev, "newBufferWithBytes:length:options:", indices, sizeof(indices), 0);
    void *rb = ((void *(*)(void *, const char *, uint64_t, uint64_t))oh2_objc_msgSend)(dev, "newBufferWithLength:options:", 64 * 64 * 4, 0);

    void *cb = msg0(q, "commandBuffer");

    /* Pass 1: Clear full RT to blue (0, 0, 255, 255) */
    {
        void *rpd = msg0(oh2_objc_get_class("MTLRenderPassDescriptor"), "renderPassDescriptor");
        void *c0 = ((void *(*)(void *, const char *, uint64_t))oh2_objc_msgSend)(msg0(rpd, "colorAttachments"), "objectAtIndexedSubscript:", 0);
        msg1v(c0, "setTexture:", rt);
        ((void (*)(void *, const char *, uint64_t))oh2_objc_msgSend)(c0, "setLoadAction:", kMTLLoadActionClear);
        ((void (*)(void *, const char *, uint64_t))oh2_objc_msgSend)(c0, "setStoreAction:", kMTLStoreActionStore);
        ((void (*)(void *, const char *, MTLClearColor))oh2_objc_msgSend)(c0, "setClearColor:", MTLClearColor{0.0, 0.0, 1.0, 1.0});
        void *enc = msg1(cb, "renderCommandEncoderWithDescriptor:", rpd);
        msg0(enc, "endEncoding");
    }

    /* Pass 2: Load existing blue pixels + Scissor center [16..48, 16..48] + Indexed Red quad */
    {
        void *rpd = msg0(oh2_objc_get_class("MTLRenderPassDescriptor"), "renderPassDescriptor");
        void *c0 = ((void *(*)(void *, const char *, uint64_t))oh2_objc_msgSend)(msg0(rpd, "colorAttachments"), "objectAtIndexedSubscript:", 0);
        msg1v(c0, "setTexture:", rt);
        ((void (*)(void *, const char *, uint64_t))oh2_objc_msgSend)(c0, "setLoadAction:", kMTLLoadActionLoad);
        ((void (*)(void *, const char *, uint64_t))oh2_objc_msgSend)(c0, "setStoreAction:", kMTLStoreActionStore);

        void *enc = msg1(cb, "renderCommandEncoderWithDescriptor:", rpd);
        msg1v(enc, "setRenderPipelineState:", pso);
        MTLViewport vp{0.0, 0.0, 64.0, 64.0, 0.0, 1.0};
        ((void (*)(void *, const char *, MTLViewport *))oh2_objc_msgSend)(enc, "setViewport:", &vp);
        MTLScissorRect sc{16, 16, 32, 32};
        ((void (*)(void *, const char *, MTLScissorRect *))oh2_objc_msgSend)(enc, "setScissorRect:", &sc);
        ((void (*)(void *, const char *, void *, uint64_t, uint64_t))oh2_objc_msgSend)(enc, "setVertexBuffer:offset:atIndex:", vb, 0, 0);
        const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        ((void (*)(void *, const char *, const void *, uint64_t, uint64_t))oh2_objc_msgSend)(enc, "setFragmentBytes:length:atIndex:", tint, sizeof(tint), 0);
        ((void (*)(void *, const char *, uint64_t, uint64_t, uint64_t, void *, uint64_t))oh2_objc_msgSend)(
            enc, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
            kMTLPrimitiveTriangle, 6, kMTLIndexUInt16, ib, 0);
        msg0(enc, "endEncoding");
    }

    /* Blit readback to CPU buffer */
    {
        void *be = msg0(cb, "blitCommandEncoder");
        MTLOrigin org{0, 0, 0};
        MTLSize sz{64, 64, 1};
        typedef void (*copy_t)(void *, const char *, void *, uint64_t, uint64_t, MTLOrigin *, MTLSize *, void *, uint64_t, uint64_t, uint64_t);
        ((copy_t)oh2_objc_msgSend)(be, "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:",
                                   rt, 0, 0, &org, &sz, rb, 0, 64 * 4, 64 * 64 * 4);
        msg0(be, "endEncoding");
    }

    msg0(cb, "commit");
    msg0(cb, "waitUntilCompleted");

    const uint8_t *px = static_cast<const uint8_t *>(msg0(rb, "contents"));
    if (!px) return false;
    /* Corner (0,0) preserved blue from Pass 1; Center (32,32) rendered red in Pass 2 */
    const uint8_t *corner = px + (2 * 64 + 2) * 4;
    const uint8_t *center = px + (32 * 64 + 32) * 4;
    bool corner_blue = (corner[0] < 10 && corner[1] < 10 && corner[2] > 240);
    bool center_red = (center[0] > 240 && center[1] < 10 && center[2] < 10);

    MtlVkStats stats{};
    mtl_vk_get_stats(&stats);
    MTLVK_LOGI("encoder_scissor_load: corner=(%u,%u,%u,%u) center=(%u,%u,%u,%u) errors=%u",
               corner[0], corner[1], corner[2], corner[3],
               center[0], center[1], center[2], center[3],
               stats.validation_errors);
    return (corner_blue && center_red && stats.indexed_draw_calls >= 1 &&
            stats.validation_errors == 0 && stats.validation_warnings == 0);
}

static bool test_metal_compute() {
    const SpirvShaderEntry *cs = nullptr;
    for (uint32_t i = 0; i < spirv_cache_count(); ++i) {
        const SpirvShaderEntry *e = spirv_cache_entry_at(i);
        if (e && e->stage == SPIRV_STAGE_COMPUTE) {
            cs = e;
            break;
        }
    }
    if (!cs) {
        MTLVK_LOGE("metal_compute: no compute shader in spirv_cache");
        return false;
    }

    void *(*create_dev)(void) = (void *(*)(void))darwin_resolve_symbol(nullptr, "_MTLCreateSystemDefaultDevice");
    void *dev = create_dev();
    void *q = msg0(dev, "newCommandQueue");

    auto *fn_impl = new MtlFunctionImpl{cs, get_or_create_shader_module(cs), 3, cs->entry_name ? cs->entry_name : "cs"};
    void *fn_obj = alloc_mtl_wrap("MTLFunction", fn_impl);
    void *err = nullptr;
    void *cpso = ((void *(*)(void *, const char *, void *, void **))oh2_objc_msgSend)(
        dev, "newComputePipelineStateWithFunction:error:", fn_obj, &err);
    if (!cpso) return false;

    void *cb = msg0(q, "commandBuffer");
    void *ce = msg0(cb, "computeCommandEncoder");
    msg1v(ce, "setComputePipelineState:", cpso);

    /* Bind buffers/textures for every slot declared by this real Global OH2 CS */
    void *dummy_tex = nullptr;
    void *td = ((void *(*)(void *, const char *, uint64_t, uint64_t, uint64_t, uint8_t))oh2_objc_msgSend)(
        oh2_objc_get_class("MTLTextureDescriptor"),
        "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", 70, 64, 64, 0);
    dummy_tex = msg1(dev, "newTextureWithDescriptor:", td);
    std::vector<uint8_t> zeros(64 * 64 * 4, 128);
    MTLRegion creg{{0, 0, 0}, {64, 64, 1}};
    ((void (*)(void *, const char *, MTLRegion *, uint64_t, const void *, uint64_t))oh2_objc_msgSend)(
        dummy_tex, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", &creg, 0, zeros.data(), 64 * 4);

    for (uint16_t i = 0; i < cs->binding_count; ++i) {
        const auto &b = cs->bindings[i];
        if (b.metal_kind == SPIRV_METAL_KIND_BUFFER) {
            uint64_t sz = std::max<uint64_t>(4096, b.buffer_size_bytes);
            void *buf = ((void *(*)(void *, const char *, uint64_t, uint64_t))oh2_objc_msgSend)(dev, "newBufferWithLength:options:", sz, 0);
            ((void (*)(void *, const char *, void *, uint64_t, uint64_t))oh2_objc_msgSend)(ce, "setBuffer:offset:atIndex:", buf, 0, b.metal_slot);
        } else if (b.metal_kind == SPIRV_METAL_KIND_TEXTURE) {
            ((void (*)(void *, const char *, void *, uint64_t))oh2_objc_msgSend)(ce, "setTexture:atIndex:", dummy_tex, b.metal_slot);
        }
    }

    MTLSize tgs{1, 1, 1};
    MTLSize tptg{cs->local_size_x ? cs->local_size_x : 64u, cs->local_size_y ? cs->local_size_y : 1u, cs->local_size_z ? cs->local_size_z : 1u};
    ((void (*)(void *, const char *, MTLSize *, MTLSize *))oh2_objc_msgSend)(
        ce, "dispatchThreadgroups:threadsPerThreadgroup:", &tgs, &tptg);
    msg0(ce, "endEncoding");
    msg0(cb, "commit");
    msg0(cb, "waitUntilCompleted");

    MtlVkStats stats{};
    mtl_vk_get_stats(&stats);
    MTLVK_LOGI("metal_compute: shader='%s' dispatches=%llu val_err=%u",
               cs->entry_name ? cs->entry_name : "?",
               (unsigned long long)stats.compute_dispatches, stats.validation_errors);
    return (stats.compute_dispatches >= 1 && stats.compute_pipelines_created >= 1 &&
            stats.validation_errors == 0 && stats.validation_warnings == 0);
}

static bool test_swapchain_resize_3x(ANativeWindow *window) {
    if (!window) return false;
    for (int i = 0; i < 3; ++i) {
        if (mtl_vk_set_surface(window) != 0) return false;
        if (!test_metal_render_pass()) return false;
    }
    MtlVkStats stats{};
    mtl_vk_get_stats(&stats);
    return (stats.surface_recreates >= 3 &&
            stats.validation_errors == 0 &&
            stats.validation_warnings == 0);
}

} // namespace

} // namespace oh2::mtlvk

extern "C" int mtl_vk_run_selftest(const char *test_name, ANativeWindow *window, const char *spirv_cache_path) {
    using namespace oh2::mtlvk;
    oh2_runtime_init("/sdcard/Android/data/com.oceanhorn2.port/files");
    if (mtl_vk_init(window, spirv_cache_path, 1) != 0) {
        MTLVK_LOGE("[M5_TEST] FAIL: %s (mtl_vk_init failed)", test_name ? test_name : "?");
        return 1;
    }
    mtl_vk_register_objc_bridges();

    bool ok = false;
    const char *name = (test_name && test_name[0]) ? test_name : "metal_objects";
    if (std::strcmp(name, "metal_objects") == 0) ok = test_metal_objects();
    else if (std::strcmp(name, "metal_render_pass") == 0) ok = test_metal_render_pass();
    else if (std::strcmp(name, "encoder_scissor_load") == 0) ok = test_encoder_scissor_load();
    else if (std::strcmp(name, "metal_compute") == 0) ok = test_metal_compute();
    else if (std::strcmp(name, "swapchain_resize_3x") == 0) ok = test_swapchain_resize_3x(window);
    else if (std::strcmp(name, "all") == 0) {
        ok = test_metal_objects() &&
             test_metal_render_pass() &&
             test_encoder_scissor_load() &&
             test_metal_compute() &&
             test_swapchain_resize_3x(window);
    }

    MtlVkStats stats{};
    mtl_vk_get_stats(&stats);
    if (ok && stats.validation_errors == 0 && stats.validation_warnings == 0) {
        MTLVK_LOGI("[M5_TEST] PASS: %s (draws=%llu idx=%llu cs=%llu presents=%llu astc=%llu val_err=0 val_warn=0)",
                   name,
                   (unsigned long long)stats.draw_calls,
                   (unsigned long long)stats.indexed_draw_calls,
                   (unsigned long long)stats.compute_dispatches,
                   (unsigned long long)stats.presents,
                   (unsigned long long)stats.astc_textures_created);
        return 0;
    }
    MTLVK_LOGE("[M5_TEST] FAIL: %s (ok=%d val_err=%u val_warn=%u)",
               name, ok ? 1 : 0, stats.validation_errors, stats.validation_warnings);
    return 1;
}
