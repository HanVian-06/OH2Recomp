#ifndef OH2_METAL_VK_INTERNAL_H
#define OH2_METAL_VK_INTERNAL_H

#include "mtl_vk.h"
#include "spirv_cache.h"
#include "vk_foundation/vk_context.h"
#include "vk_foundation/staging_ring.h"
#include "vk_foundation/astc_texture.h"
#include "../loader/macho_loader.h"
#include "../runtime/oh2_runtime.h"

#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define MTLVK_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "OH2_MTLVK", __VA_ARGS__)
#define MTLVK_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  "OH2_MTLVK", __VA_ARGS__)
#define MTLVK_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OH2_MTLVK", __VA_ARGS__)

namespace oh2::mtlvk {

/* ============================================================================
 * Apple ARM64 Metal ABI Struct Layouts
 * ============================================================================ */
struct MTLOrigin {
    uint64_t x;
    uint64_t y;
    uint64_t z;
};
struct MTLSize {
    uint64_t width;
    uint64_t height;
    uint64_t depth;
};
struct MTLRegion {
    MTLOrigin origin;
    MTLSize   size;
};
struct MTLViewport {
    double originX;
    double originY;
    double width;
    double height;
    double znear;
    double zfar;
};
struct MTLScissorRect {
    uint64_t x;
    uint64_t y;
    uint64_t width;
    uint64_t height;
};
struct MTLClearColor {
    double red;
    double green;
    double blue;
    double alpha;
};
struct NSRange64 {
    uint64_t location;
    uint64_t length;
};
struct CGSize64 {
    double width;
    double height;
};
struct MTLSizeAndAlign {
    uint64_t size;
    uint64_t align;
};

static_assert(sizeof(MTLOrigin) == 24, "MTLOrigin must be 24 bytes");
static_assert(sizeof(MTLSize) == 24, "MTLSize must be 24 bytes");
static_assert(sizeof(MTLRegion) == 48, "MTLRegion must be 48 bytes");
static_assert(sizeof(MTLViewport) == 48, "MTLViewport must be 48 bytes");
static_assert(sizeof(MTLScissorRect) == 32, "MTLScissorRect must be 32 bytes");
static_assert(sizeof(MTLClearColor) == 32, "MTLClearColor must be 32 bytes");
static_assert(sizeof(NSRange64) == 16, "NSRange64 must be 16 bytes");
static_assert(sizeof(CGSize64) == 16, "CGSize64 must be 16 bytes");

/* ============================================================================
 * Metal Enums (matching Apple iOS Metal 2.0 SDK values)
 * ============================================================================ */
constexpr uint64_t kMTLTextureType1D        = 0;
constexpr uint64_t kMTLTextureType1DArray   = 1;
constexpr uint64_t kMTLTextureType2D        = 2;
constexpr uint64_t kMTLTextureType2DArray   = 3;
constexpr uint64_t kMTLTextureType2DMS      = 4;
constexpr uint64_t kMTLTextureTypeCube      = 5;
constexpr uint64_t kMTLTextureTypeCubeArray = 6;
constexpr uint64_t kMTLTextureType3D        = 7;

constexpr uint64_t kMTLLoadActionDontCare   = 0;
constexpr uint64_t kMTLLoadActionLoad       = 1;
constexpr uint64_t kMTLLoadActionClear      = 2;

constexpr uint64_t kMTLStoreActionDontCare  = 0;
constexpr uint64_t kMTLStoreActionStore     = 1;
constexpr uint64_t kMTLStoreActionMultisampleResolve = 2;

constexpr uint64_t kMTLPrimitivePoint         = 0;
constexpr uint64_t kMTLPrimitiveLine          = 1;
constexpr uint64_t kMTLPrimitiveLineStrip     = 2;
constexpr uint64_t kMTLPrimitiveTriangle      = 3;
constexpr uint64_t kMTLPrimitiveTriangleStrip = 4;

constexpr uint64_t kMTLIndexUInt16 = 0;
constexpr uint64_t kMTLIndexUInt32 = 1;

constexpr uint64_t kMTLCullNone  = 0;
constexpr uint64_t kMTLCullFront = 1;
constexpr uint64_t kMTLCullBack  = 2;

constexpr uint64_t kMTLWindingCW  = 0;
constexpr uint64_t kMTLWindingCCW = 1;

constexpr uint64_t kMaxBuffers  = 31;
constexpr uint64_t kMaxTextures = 32;
constexpr uint64_t kMaxSamplers = 16;
constexpr uint64_t kMaxColorAttachments = 8;

/* ============================================================================
 * C++ Backing Structures (each ObjC object starts with `void *isa; Impl *impl;`)
 * ============================================================================ */
struct MtlBufferImpl {
    VkBuffer      buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    uint64_t      length = 0;
    uint64_t      options = 0;
    void         *mapped = nullptr;
    bool          host_coherent = false;
    std::string   label;
};

struct MtlTextureDescriptorImpl {
    uint64_t texture_type = kMTLTextureType2D;
    uint64_t pixel_format = 80; /* BGRA8Unorm */
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipmap_level_count = 1;
    uint32_t sample_count = 1;
    uint32_t array_length = 1;
    uint64_t resource_options = 0;
    uint64_t storage_mode = 2; /* Private */
    uint64_t cpu_cache_mode = 0;
    uint64_t usage = 0x01;     /* ShaderRead */
    uint8_t  allow_gpu_optimized = 1;
};

struct MtlTextureImpl {
    VkImage         image = VK_NULL_HANDLE;
    VmaAllocation   allocation = VK_NULL_HANDLE;
    VkImageView     view = VK_NULL_HANDLE;
    VkFormat        vk_format = VK_FORMAT_B8G8R8A8_UNORM;
    uint64_t        mtl_pixel_format = 80;
    uint64_t        texture_type = kMTLTextureType2D;
    uint32_t        width = 1;
    uint32_t        height = 1;
    uint32_t        depth = 1;
    uint32_t        mip_levels = 1;
    uint32_t        array_layers = 1;
    uint32_t        sample_count = 1;
    VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
    std::vector<VkImageLayout> mip_layouts;
    bool            owns_image = true;
    bool            owns_view = true;
    bool            is_swapchain = false;
    uint32_t        swapchain_image_idx = UINT32_MAX;
    void           *parent_tex_obj = nullptr;
    void           *parent_buffer_obj = nullptr;
    bool            retains_parent_tex = false;
    bool            retains_parent_buf = false;
    uint64_t        buffer_offset = 0;
    uint64_t        buffer_bytes_per_row = 0;
    uint64_t        cpu_cache_mode = 0;
    uint64_t        storage_mode = 2;
    uint64_t        usage = 1;
    uint8_t         framebuffer_only = 0;
    uint64_t        allocated_size = 0;
    std::string     label;
};

struct MtlDrawableImpl {
    void       *layer_obj = nullptr;
    void       *texture_obj = nullptr;
    uint32_t    swapchain_idx = 0;
    uint64_t    drawable_id = 0;
    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    bool        acquired = false;
};

struct MtlMetalLayerImpl {
    void    *device_obj = nullptr;
    uint64_t pixel_format = 80; /* BGRA8Unorm */
    uint8_t  framebuffer_only = 1;
    uint8_t  presents_with_tx = 0;
    double   drawable_width = 2340.0;
    double   drawable_height = 1080.0;
    double   contents_scale = 2.0;
    bool     has_active_drawable = false;
    uint32_t active_sc_idx = 0;
    void    *active_drawable_obj = nullptr;
};

struct MtlSamplerDescriptorImpl {
    uint64_t min_filter = 0;
    uint64_t mag_filter = 0;
    uint64_t mip_filter = 0;
    uint64_t s_addr = 0;
    uint64_t t_addr = 0;
    uint64_t r_addr = 0;
    uint64_t max_anisotropy = 1;
    float    lod_min = 0.0f;
    float    lod_max = 1000.0f;
    uint64_t compare_func = 0; /* Never */
    uint8_t  normalized_coords = 1;
    std::string label;
};

struct MtlSamplerImpl {
    VkSampler sampler = VK_NULL_HANDLE;
};

struct MtlStencilDescriptorImpl {
    uint64_t compare_fn = 7; /* Always */
    uint64_t stencil_fail_op = 0;
    uint64_t depth_fail_op = 0;
    uint64_t depth_stencil_pass_op = 0;
    uint32_t read_mask = 0xFFFFFFFFu;
    uint32_t write_mask = 0xFFFFFFFFu;
};

struct MtlDepthStencilDescriptorImpl {
    uint64_t depth_compare_fn = 7; /* Always */
    uint8_t  depth_write_enabled = 0;
    MtlStencilDescriptorImpl front;
    MtlStencilDescriptorImpl back;
    void *front_wrap = nullptr;
    void *back_wrap = nullptr;
    std::string label;
};

struct MtlDepthStencilStateImpl {
    MtlDepthStencilDescriptorImpl desc;
};

struct MtlLibraryImpl {
    const SpirvShaderEntry *single_entry = nullptr;
    bool                    is_default_library = false;
    std::string             label;
};

struct MtlFunctionImpl {
    const SpirvShaderEntry *entry = nullptr;
    VkShaderModule          module = VK_NULL_HANDLE;
    uint64_t                function_type = 1; /* 1=Vertex, 2=Fragment, 3=Kernel */
    std::string             name;
};

struct MtlPipelineColorAttachDesc {
    uint64_t pixel_format = 0;
    uint8_t  blending_enabled = 0;
    uint64_t src_rgb = 1;   /* One */
    uint64_t dst_rgb = 0;   /* Zero */
    uint64_t rgb_op = 0;    /* Add */
    uint64_t src_alpha = 1;
    uint64_t dst_alpha = 0;
    uint64_t alpha_op = 0;
    uint64_t write_mask = 0xF;
};

struct MtlVertexAttrDesc {
    uint64_t format = 0;
    uint64_t offset = 0;
    uint64_t buffer_index = 0;
};

struct MtlVertexLayoutDesc {
    uint64_t stride = 0;
    uint64_t step_function = 1; /* PerVertex */
    uint64_t step_rate = 1;
};

struct MtlVertexDescriptorImpl {
    MtlVertexAttrDesc   attrs[32]{};
    MtlVertexLayoutDesc layouts[32]{};
    void               *attr_array_obj = nullptr;
    void               *layout_array_obj = nullptr;
    void               *attr_wraps[32]{};
    void               *layout_wraps[32]{};
};

struct MtlRenderPipelineDescImpl {
    void     *vertex_fn_obj = nullptr;
    void     *fragment_fn_obj = nullptr;
    void     *vertex_desc_obj = nullptr;
    MtlPipelineColorAttachDesc color[kMaxColorAttachments]{};
    void     *color_array_obj = nullptr;
    void     *color_wraps[kMaxColorAttachments]{};
    uint64_t  depth_pixel_format = 0;
    uint64_t  stencil_pixel_format = 0;
    uint64_t  sample_count = 1;
    uint8_t   alpha_to_coverage = 0;
    std::string label;
};

struct MtlRenderPipelineStateImpl {
    MtlRenderPipelineDescImpl desc;
    const SpirvShaderEntry *vs = nullptr;
    const SpirvShaderEntry *ps = nullptr;
    VkShaderModule vs_module = VK_NULL_HANDLE;
    VkShaderModule ps_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set1_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    uint32_t vertex_buffer_mask = 0;
    MtlVertexDescriptorImpl vtx_desc_copy{};
    std::unordered_map<uint64_t, VkPipeline> specialized_pipelines;
};

struct MtlComputePipelineStateImpl {
    const SpirvShaderEntry *cs = nullptr;
    VkShaderModule cs_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint64_t max_threads_per_tg = 512;
    uint64_t thread_exec_width = 32;
};

struct MtlRenderPassColorAttach {
    void         *texture_obj = nullptr;
    void         *resolve_texture_obj = nullptr;
    uint64_t      level = 0;
    uint64_t      slice = 0;
    uint64_t      resolve_level = 0;
    uint64_t      resolve_slice = 0;
    uint64_t      load_action = kMTLLoadActionDontCare;
    uint64_t      store_action = kMTLStoreActionStore;
    MTLClearColor clear_color{0.0, 0.0, 0.0, 1.0};
};

struct MtlRenderPassDepthAttach {
    void    *texture_obj = nullptr;
    void    *resolve_texture_obj = nullptr;
    uint64_t level = 0;
    uint64_t slice = 0;
    uint64_t resolve_level = 0;
    uint64_t resolve_slice = 0;
    uint64_t load_action = kMTLLoadActionDontCare;
    uint64_t store_action = kMTLStoreActionDontCare;
    double   clear_depth = 1.0;
};

struct MtlRenderPassStencilAttach {
    void    *texture_obj = nullptr;
    uint64_t level = 0;
    uint64_t slice = 0;
    uint64_t load_action = kMTLLoadActionDontCare;
    uint64_t store_action = kMTLStoreActionDontCare;
    uint32_t clear_stencil = 0;
};

struct MtlRenderPassDescImpl {
    MtlRenderPassColorAttach   color[kMaxColorAttachments]{};
    MtlRenderPassDepthAttach   depth{};
    MtlRenderPassStencilAttach stencil{};
    void                      *color_array_obj = nullptr;
    void                      *color_wraps[kMaxColorAttachments]{};
    void                      *depth_wrap = nullptr;
    void                      *stencil_wrap = nullptr;
    uint32_t                   rt_width = 0;
    uint32_t                   rt_height = 0;
};

struct MtlFenceImpl {
    uint64_t signal_count = 0;
    std::string label;
};

struct MtlHeapImpl {
    uint64_t size = 0;
    uint64_t used = 0;
    uint64_t storage_mode = 2;
    std::string label;
};

struct MtlStageBindings {
    VkBuffer buffer[kMaxBuffers]{};
    uint64_t buffer_offset[kMaxBuffers]{};
    uint64_t buffer_range[kMaxBuffers]{};
    uint64_t buffer_total_len[kMaxBuffers]{};
    bool     buffer_bound[kMaxBuffers]{};

    VkImageView tex_view[kMaxTextures]{};
    void       *tex_obj[kMaxTextures]{};
    bool        tex_is_depth[kMaxTextures]{};
    bool        tex_bound[kMaxTextures]{};

    VkSampler sampler[kMaxSamplers]{};
    bool      sampler_bound[kMaxSamplers]{};
};

struct MtlCommandBufferImpl {
    uint32_t frame_idx = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool recording = false;
    bool committed = false;
    VkSemaphore acquire_wait_sem = VK_NULL_HANDLE;
    bool present_requested = false;
    uint32_t present_sc_idx = 0;
    void *present_tex_obj = nullptr;
    uint64_t status = 0; /* 0=NotEnqueued, 2=Committed, 4=Completed */
    std::string label;
    std::vector<void *> scheduled_blocks;
    std::vector<void *> completed_blocks;
    void *active_encoder = nullptr;
};

struct MtlRenderEncoderImpl {
    void *cmd_buf_obj = nullptr;
    MtlCommandBufferImpl *cmd_buf = nullptr;
    MtlRenderPassDescImpl pass_desc{};
    uint32_t render_w = 1;
    uint32_t render_h = 1;
    uint32_t color_count = 0;
    VkFormat color_formats[kMaxColorAttachments]{};
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkFormat stencil_format = VK_FORMAT_UNDEFINED;
    VkRenderPass fallback_render_pass = VK_NULL_HANDLE;
    VkFramebuffer fallback_framebuffer = VK_NULL_HANDLE;
    std::vector<VkImageView> attachment_views_to_destroy;

    void *pipeline_state_obj = nullptr;
    void *depth_stencil_obj = nullptr;
    uint64_t cull_mode = kMTLCullNone;
    uint64_t front_winding = kMTLWindingCW;
    uint64_t fill_mode = 0; /* Fill */
    uint32_t stencil_ref = 0;
    float depth_bias = 0.0f;
    float depth_slope = 0.0f;
    float depth_clamp = 0.0f;
    float blend_constants[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    MTLViewport viewport{0, 0, 1, 1, 0, 1};
    bool has_viewport = false;
    MTLScissorRect scissor{0, 0, 1, 1};
    bool has_scissor = false;

    MtlStageBindings vs{};
    MtlStageBindings ps{};
    bool inside_render_pass = false;
};

struct MtlComputeEncoderImpl {
    void *cmd_buf_obj = nullptr;
    MtlCommandBufferImpl *cmd_buf = nullptr;
    void *pipeline_state_obj = nullptr;
    MtlStageBindings cs{};
};

struct MtlBlitEncoderImpl {
    void *cmd_buf_obj = nullptr;
    MtlCommandBufferImpl *cmd_buf = nullptr;
};

/* ObjC wrapper object prefix: every Metal ObjC object has `void *isa; T *impl;` */
template <typename T>
struct MtlObjWrap {
    void *isa;
    T    *impl;
};

/* ============================================================================
 * Singleton Context State
 * ============================================================================ */
static constexpr uint32_t kMaxFramesInFlight = 2;

struct FrameSyncSlot {
    VkCommandPool                 cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer               cmd = VK_NULL_HANDLE;
    VkDescriptorPool              desc_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> overflow_desc_pools;
    uint32_t                      active_desc_pool_idx = 0;
    VkSemaphore                   image_available_sem = VK_NULL_HANDLE;
    VkFence                       in_flight_fence = VK_NULL_HANDLE;
    VkBuffer                      inline_buf = VK_NULL_HANDLE;
    VmaAllocation                 inline_alloc = VK_NULL_HANDLE;
    uint8_t                      *inline_mapped = nullptr;
    uint64_t                      inline_cursor = 0;
    uint64_t                      inline_capacity = 0;
    bool                          submitted = false;
};

struct DeferredDestroy {
    enum Kind : uint8_t {
        BUFFER = 0,
        IMAGE = 1,
        IMAGE_VIEW = 2,
        FRAMEBUFFER = 3,
        SAMPLER = 4,
    };
    Kind          kind = BUFFER;
    VkBuffer      buffer = VK_NULL_HANDLE;
    VkImage       image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView   image_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkSampler     sampler = VK_NULL_HANDLE;
    uint32_t      frames_remaining = 3;
};

struct MtlVkState {
    std::mutex                   mutex;
    bool                         initialized = false;
    bool                         validation_enabled = false;
    oh2::vk::VkContext           vk_ctx;
    oh2::vk::StagingRingBuffer   staging;
    VkPipelineCache              pipeline_cache = VK_NULL_HANDLE;

    VkCommandPool                upload_pool = VK_NULL_HANDLE;
    VkFence                      upload_fence = VK_NULL_HANDLE;

    FrameSyncSlot                frames[kMaxFramesInFlight]{};
    VkSemaphore                  acquire_sem_pool[8]{};
    uint32_t                     next_acquire_sem = 0;
    std::vector<VkSemaphore>     render_finished_sems;
    std::vector<VkImageLayout>   swapchain_layouts;
    std::vector<void *>          swapchain_tex_objs;
    std::vector<void *>          swapchain_drawable_objs;
    std::vector<DeferredDestroy> deferred_destroys;
    uint32_t                     next_frame_idx = 0;
    bool                         swapchain_out_of_date = false;

    /* Fallback 1x1 white texture, dummy sampler, and dummy 4 KiB UBO/SSBO for unbound shader slots */
    VkImage                      dummy_image = VK_NULL_HANDLE;
    VmaAllocation                dummy_image_alloc = VK_NULL_HANDLE;
    VkImageView                  dummy_image_view = VK_NULL_HANDLE;
    VkImage                      dummy_depth_image = VK_NULL_HANDLE;
    VmaAllocation                dummy_depth_alloc = VK_NULL_HANDLE;
    VkImageView                  dummy_depth_view = VK_NULL_HANDLE;
    VkSampler                    dummy_sampler = VK_NULL_HANDLE;
    VkBuffer                     dummy_buffer = VK_NULL_HANDLE;
    VmaAllocation                dummy_buffer_alloc = VK_NULL_HANDLE;

    /* Shader module & VkRenderPass caches */
    std::unordered_map<uint64_t, VkShaderModule> shader_modules;
    std::unordered_map<uint64_t, VkRenderPass>   render_pass_cache;
    std::unordered_map<std::string, uint64_t>    compat_render_pass_cache;

    /* Singleton ObjC objects */
    void *device_obj = nullptr;
    void *default_queue_obj = nullptr;
    void *shared_layer_obj = nullptr;
    void *default_library_obj = nullptr;

    /* Telemetry counters */
    std::atomic<uint64_t> draw_calls{0};
    std::atomic<uint64_t> indexed_draw_calls{0};
    std::atomic<uint64_t> compute_dispatches{0};
    std::atomic<uint64_t> blit_commands{0};
    std::atomic<uint64_t> command_buffers_committed{0};
    std::atomic<uint64_t> presents{0};
    std::atomic<uint64_t> graphics_pipelines_created{0};
    std::atomic<uint64_t> blended_pipelines_created{0};
    std::atomic<uint64_t> compute_pipelines_created{0};
    std::atomic<uint64_t> astc_textures_created{0};
    std::atomic<uint64_t> astc_cpu_decompress_count{0};
    std::atomic<uint64_t> surface_recreates{0};
    std::atomic<int64_t>  live_buffers{0};
    std::atomic<int64_t>  live_buffer_bytes{0};
    std::atomic<int64_t>  live_textures{0};
    std::atomic<int64_t>  live_texture_bytes{0};
    std::atomic<int64_t>  live_cmdbufs{0};
    std::atomic<int64_t>  live_encoders{0};
    std::atomic<uint64_t> mtlb_cache_hits{0};
    std::atomic<uint64_t> mtlb_cache_misses{0};

    /* Cached snapshot image for Metal programmable blending / framebuffer fetch (color(0)) */
    VkImage       fb_fetch_image = VK_NULL_HANDLE;
    VmaAllocation fb_fetch_alloc = VK_NULL_HANDLE;
    VkImageView   fb_fetch_view = VK_NULL_HANDLE;
    uint32_t      fb_fetch_w = 0;
    uint32_t      fb_fetch_h = 0;
    VkFormat      fb_fetch_fmt = VK_FORMAT_UNDEFINED;
};

MtlVkState &get_state();

/* ============================================================================
 * Cross-file helpers inside src/metal_vk/
 * ============================================================================ */
void   register_device_objc_methods();
void   register_resource_objc_methods();
void   register_pipeline_objc_methods();
void   register_encoder_objc_methods();

VkFormat mtl_pixel_format_to_vk(uint64_t mtl_fmt, bool *out_is_astc = nullptr, bool *out_is_depth = nullptr, bool *out_has_stencil = nullptr);
uint32_t mtl_pixel_format_block_size(VkFormat fmt, uint32_t *out_bw, uint32_t *out_bh);
VkShaderModule get_or_create_shader_module(const SpirvShaderEntry *entry);
VkShaderModule create_shader_module_from_words(const uint32_t *words, size_t byte_size);

void transition_image_layout(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                             uint32_t base_mip, uint32_t mip_count,
                             uint32_t base_layer, uint32_t layer_count,
                             VkImageLayout old_layout, VkImageLayout new_layout);

bool execute_immediate_upload(void (*record_fn)(VkCommandBuffer, void *), void *user_data);

void *alloc_mtl_wrap(const char *cls_name, void *impl_ptr);
void *alloc_mtl_wrap_managed(const char *cls_name, void *impl_ptr, void (*dealloc_fn)(void *), bool autorelease_obj = false);
template <typename T>
inline T *unwrap_mtl(void *obj) {
    if (!obj) return nullptr;
    return ((MtlObjWrap<T> *)obj)->impl;
}

VkDescriptorPool create_frame_descriptor_pool(VkDevice dev);
void enqueue_deferred_buffer(VkBuffer buf, VmaAllocation alloc);
void enqueue_deferred_image(VkImage img, VmaAllocation alloc, VkImageView view);
void enqueue_deferred_view(VkImageView view);
void enqueue_deferred_framebuffer(VkFramebuffer fb);
void enqueue_deferred_sampler(VkSampler smp);
void tick_deferred_destroys(bool force_all = false);

void *wrap_swapchain_image_as_mtl_texture(uint32_t sc_idx);
void  invoke_objc_block_1(void *block, void *arg1);
void *create_ns_error(const char *msg);

/* Pipeline & render pass helpers (mtl_vk_pipeline.cpp) */
VkRenderPass get_or_create_render_pass(const VkFormat *color_fmts, uint32_t color_count,
                                       const uint64_t *color_loads, const uint64_t *color_stores,
                                       VkFormat depth_fmt, uint64_t depth_load, uint64_t depth_store,
                                       bool for_compat);
VkPipeline   get_or_create_specialized_graphics_pipeline(MtlRenderPipelineStateImpl *pso,
                                                         MtlDepthStencilStateImpl *dss,
                                                         uint64_t primitive_type,
                                                         uint64_t cull_mode,
                                                         uint64_t front_winding,
                                                         uint64_t fill_mode,
                                                         const VkFormat *color_fmts,
                                                         uint32_t color_count,
                                                         VkFormat depth_fmt,
                                                         VkFormat stencil_fmt,
                                                         VkRenderPass compat_rp);
VkDescriptorSetLayout create_stage_descriptor_layout(const SpirvShaderEntry *entry, VkShaderStageFlags stage_flag);

/* Frame synchronization & command execution */
VkCommandBuffer mtl_begin_command_buffer_internal(MtlCommandBufferImpl *cb);
bool            mtl_acquire_swapchain_image_internal(MtlMetalLayerImpl *layer, uint32_t *out_sc_idx, VkSemaphore *out_wait_sem);
bool            mtl_commit_command_buffer_internal(void *cb_obj, bool wait_completed);
bool            mtl_alloc_inline_bytes(MtlCommandBufferImpl *cb, const void *data, uint64_t len, VkBuffer *out_buf, uint64_t *out_off);

} // namespace oh2::mtlvk

#endif /* OH2_METAL_VK_INTERNAL_H */
