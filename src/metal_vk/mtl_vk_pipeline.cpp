#include "mtl_vk_internal.h"

#include <algorithm>
#include <cstring>

namespace oh2::mtlvk {

namespace {

struct ArrayProxy {
    void *parent;
};

static void mtl_array_proxy_dealloc(void *self) {
    delete unwrap_mtl<ArrayProxy>(self);
}

static void mtl_wrap_only_noop_dealloc(void *self) {
    (void)self;
}

static void mtl_va_owned_dealloc(void *self) {
    delete unwrap_mtl<MtlVertexAttrDesc>(self);
}

static void mtl_vl_owned_dealloc(void *self) {
    delete unwrap_mtl<MtlVertexLayoutDesc>(self);
}

static void mtl_pca_owned_dealloc(void *self) {
    delete unwrap_mtl<MtlPipelineColorAttachDesc>(self);
}

static void mtl_rpc_owned_dealloc(void *self) {
    delete unwrap_mtl<MtlRenderPassColorAttach>(self);
}

static void mtl_rpda_owned_dealloc(void *self) {
    delete unwrap_mtl<MtlRenderPassDepthAttach>(self);
}

static void mtl_rpsa_owned_dealloc(void *self) {
    delete unwrap_mtl<MtlRenderPassStencilAttach>(self);
}

static void mtl_rpd_dealloc(void *self) {
    auto *d = unwrap_mtl<MtlRenderPassDescImpl>(self);
    if (!d) return;
    if (d->color_array_obj) oh2_objc_release_object(d->color_array_obj);
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        if (d->color_wraps[i]) oh2_objc_release_object(d->color_wraps[i]);
    }
    if (d->depth_wrap) oh2_objc_release_object(d->depth_wrap);
    if (d->stencil_wrap) oh2_objc_release_object(d->stencil_wrap);
    delete d;
}

static void mtl_vd_dealloc(void *self) {
    auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(self);
    if (!vd) return;
    if (vd->attr_array_obj) oh2_objc_release_object(vd->attr_array_obj);
    if (vd->layout_array_obj) oh2_objc_release_object(vd->layout_array_obj);
    for (int i = 0; i < 32; ++i) {
        if (vd->attr_wraps[i]) oh2_objc_release_object(vd->attr_wraps[i]);
        if (vd->layout_wraps[i]) oh2_objc_release_object(vd->layout_wraps[i]);
    }
    delete vd;
}

static void mtl_rpl_dealloc(void *self) {
    auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(self);
    if (!d) return;
    if (d->color_array_obj) oh2_objc_release_object(d->color_array_obj);
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
        if (d->color_wraps[i]) oh2_objc_release_object(d->color_wraps[i]);
    }
    delete d;
}

/* Indexed descriptor array proxies (`colorAttachments[i]`, `attributes[i]`, `layouts[i]`) */
static void *mtl_rp_color_arr_subscript(void *self, const char *sel, uint64_t idx) {
    (void)sel;
    auto *p = unwrap_mtl<ArrayProxy>(self);
    auto *rpd = unwrap_mtl<MtlRenderPassDescImpl>(p ? p->parent : nullptr);
    if (!rpd || idx >= kMaxColorAttachments) return nullptr;
    if (!rpd->color_wraps[idx]) {
        rpd->color_wraps[idx] = alloc_mtl_wrap_managed(
            "MTLRenderPassColorAttachmentDescriptor", &rpd->color[idx], &mtl_wrap_only_noop_dealloc, false);
    }
    return rpd->color_wraps[idx];
}

static void mtl_rp_color_arr_set(void *self, const char *sel, void *obj, uint64_t idx) {
    (void)sel;
    auto *p = unwrap_mtl<ArrayProxy>(self);
    auto *rpd = unwrap_mtl<MtlRenderPassDescImpl>(p ? p->parent : nullptr);
    if (!rpd || idx >= kMaxColorAttachments) return;
    if (auto *src = obj ? unwrap_mtl<MtlRenderPassColorAttach>(obj) : nullptr) {
        rpd->color[idx] = *src;
    } else {
        rpd->color[idx] = MtlRenderPassColorAttach{};
    }
}

static void *mtl_rpd_color_arr_subscript(void *self, const char *sel, uint64_t idx) {
    (void)sel;
    auto *p = unwrap_mtl<ArrayProxy>(self);
    auto *desc = unwrap_mtl<MtlRenderPipelineDescImpl>(p ? p->parent : nullptr);
    if (!desc || idx >= kMaxColorAttachments) return nullptr;
    if (!desc->color_wraps[idx]) {
        desc->color_wraps[idx] = alloc_mtl_wrap_managed(
            "MTLRenderPipelineColorAttachmentDescriptor", &desc->color[idx], &mtl_wrap_only_noop_dealloc, false);
    }
    return desc->color_wraps[idx];
}

static void mtl_rpd_color_arr_set(void *self, const char *sel, void *obj, uint64_t idx) {
    (void)sel;
    auto *p = unwrap_mtl<ArrayProxy>(self);
    auto *desc = unwrap_mtl<MtlRenderPipelineDescImpl>(p ? p->parent : nullptr);
    if (!desc || idx >= kMaxColorAttachments) return;
    if (auto *src = obj ? unwrap_mtl<MtlPipelineColorAttachDesc>(obj) : nullptr) {
        desc->color[idx] = *src;
    } else {
        desc->color[idx] = MtlPipelineColorAttachDesc{};
    }
}

static void *mtl_vd_attr_arr_subscript(void *self, const char *sel, uint64_t idx) {
    (void)sel;
    auto *p = unwrap_mtl<ArrayProxy>(self);
    auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(p ? p->parent : nullptr);
    if (!vd || idx >= 31) return nullptr;
    if (!vd->attr_wraps[idx]) {
        vd->attr_wraps[idx] = alloc_mtl_wrap_managed(
            "MTLVertexAttributeDescriptor", &vd->attrs[idx], &mtl_wrap_only_noop_dealloc, false);
    }
    return vd->attr_wraps[idx];
}

static void mtl_vd_attr_arr_set(void *self, const char *sel, void *obj, uint64_t idx) {
    (void)sel;
    auto *p = unwrap_mtl<ArrayProxy>(self);
    auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(p ? p->parent : nullptr);
    if (!vd || idx >= 31) return;
    if (obj) {
        if (auto *src = unwrap_mtl<MtlVertexAttrDesc>(obj)) {
            vd->attrs[idx] = *src;
        }
    } else {
        vd->attrs[idx] = MtlVertexAttrDesc{};
    }
}

static void *mtl_vd_layout_arr_subscript(void *self, const char *sel, uint64_t idx) {
    (void)sel;
    auto *p = unwrap_mtl<ArrayProxy>(self);
    auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(p ? p->parent : nullptr);
    if (!vd || idx >= 31) return nullptr;
    if (!vd->layout_wraps[idx]) {
        vd->layout_wraps[idx] = alloc_mtl_wrap_managed(
            "MTLVertexBufferLayoutDescriptor", &vd->layouts[idx], &mtl_wrap_only_noop_dealloc, false);
    }
    return vd->layout_wraps[idx];
}

static void mtl_vd_layout_arr_set(void *self, const char *sel, void *obj, uint64_t idx) {
    (void)sel;
    auto *p = unwrap_mtl<ArrayProxy>(self);
    auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(p ? p->parent : nullptr);
    if (!vd || idx >= 31) return;
    if (obj) {
        if (auto *src = unwrap_mtl<MtlVertexLayoutDesc>(obj)) {
            vd->layouts[idx] = *src;
        }
    } else {
        vd->layouts[idx] = MtlVertexLayoutDesc{};
    }
}

/* MTLRenderPassDescriptor & attachments */
static void *mtl_rpd_cls_desc(void *cls, const char *sel) {
    (void)cls; (void)sel;
    return alloc_mtl_wrap_managed("MTLRenderPassDescriptor", new MtlRenderPassDescImpl(), &mtl_rpd_dealloc, true);
}
static void *mtl_rpd_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlRenderPassDescImpl> *>(s);
    if (w && !w->impl) w->impl = new MtlRenderPassDescImpl();
    if (w) oh2_objc_set_dealloc(w, &mtl_rpd_dealloc);
    return s;
}
static void *mtl_rpd_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlRenderPassDescImpl>(s);
    auto *cp = src ? new MtlRenderPassDescImpl(*src) : new MtlRenderPassDescImpl();
    cp->color_array_obj = nullptr;
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) cp->color_wraps[i] = nullptr;
    cp->depth_wrap = nullptr;
    cp->stencil_wrap = nullptr;
    return alloc_mtl_wrap_managed("MTLRenderPassDescriptor", cp, &mtl_rpd_dealloc, false);
}
static void *mtl_rpd_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_rpd_copy(s, sel); }
static void *mtl_rpd_colorAttachments(void *s, const char *sel) {
    (void)sel;
    auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s);
    if (!d) return nullptr;
    if (!d->color_array_obj) {
        d->color_array_obj = alloc_mtl_wrap_managed(
            "MTLRenderPassColorAttachmentDescriptorArray", new ArrayProxy{s}, &mtl_array_proxy_dealloc, false);
    }
    return d->color_array_obj;
}
static void *mtl_rpd_depthAttachment(void *s, const char *sel) {
    (void)sel;
    auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s);
    if (!d) return nullptr;
    if (!d->depth_wrap) {
        d->depth_wrap = alloc_mtl_wrap_managed(
            "MTLRenderPassDepthAttachmentDescriptor", &d->depth, &mtl_wrap_only_noop_dealloc, false);
    }
    return d->depth_wrap;
}
static void mtl_rpd_setDepthAttachment(void *s, const char *sel, void *a) {
    (void)sel;
    auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s);
    auto *src = unwrap_mtl<MtlRenderPassDepthAttach>(a);
    if (d && src) d->depth = *src;
}
static void *mtl_rpd_stencilAttachment(void *s, const char *sel) {
    (void)sel;
    auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s);
    if (!d) return nullptr;
    if (!d->stencil_wrap) {
        d->stencil_wrap = alloc_mtl_wrap_managed(
            "MTLRenderPassStencilAttachmentDescriptor", &d->stencil, &mtl_wrap_only_noop_dealloc, false);
    }
    return d->stencil_wrap;
}
static void mtl_rpd_setStencilAttachment(void *s, const char *sel, void *a) {
    (void)sel;
    auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s);
    auto *src = unwrap_mtl<MtlRenderPassStencilAttach>(a);
    if (d && src) d->stencil = *src;
}
static void mtl_rpd_setRTW(void *s, const char *sel, uint64_t w) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s)) d->rt_width = (uint32_t)w; }
static void mtl_rpd_setRTH(void *s, const char *sel, uint64_t h) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s)) d->rt_height = (uint32_t)h; }

static void *mtl_rpc_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlRenderPassColorAttach> *>(s);
    if (w && !w->impl) w->impl = new MtlRenderPassColorAttach();
    if (w) oh2_objc_set_dealloc(w, &mtl_rpc_owned_dealloc);
    return s;
}
static void *mtl_rpc_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlRenderPassColorAttach>(s);
    auto *cp = src ? new MtlRenderPassColorAttach(*src) : new MtlRenderPassColorAttach();
    return alloc_mtl_wrap_managed("MTLRenderPassColorAttachmentDescriptor", cp, &mtl_rpc_owned_dealloc, false);
}
static void *mtl_rpc_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_rpc_copy(s, sel); }
static void *mtl_rpc_tex(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->texture_obj : nullptr; }
static void  mtl_rpc_setTex(void *s, const char *sel, void *t) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->texture_obj = t; }
static void *mtl_rpc_resTex(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->resolve_texture_obj : nullptr; }
static void  mtl_rpc_setResTex(void *s, const char *sel, void *t) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->resolve_texture_obj = t; }
static void  mtl_rpc_setLvl(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->level = v; }
static void  mtl_rpc_setSlc(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->slice = v; }
static uint64_t mtl_rpc_load(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->load_action : 0; }
static void  mtl_rpc_setLoad(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->load_action = v; }
static uint64_t mtl_rpc_store(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->store_action : 1; }
static void  mtl_rpc_setStore(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->store_action = v; }
static void  mtl_rpc_setClear(void *s, const char *sel, MTLClearColor cc) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->clear_color = cc; }
static MTLClearColor mtl_rpc_clear(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->clear_color : MTLClearColor{0,0,0,1}; }

static void *mtl_rpda_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlRenderPassDepthAttach> *>(s);
    if (w && !w->impl) w->impl = new MtlRenderPassDepthAttach();
    if (w) oh2_objc_set_dealloc(w, &mtl_rpda_owned_dealloc);
    return s;
}
static void *mtl_rpda_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlRenderPassDepthAttach>(s);
    auto *cp = src ? new MtlRenderPassDepthAttach(*src) : new MtlRenderPassDepthAttach();
    return alloc_mtl_wrap_managed("MTLRenderPassDepthAttachmentDescriptor", cp, &mtl_rpda_owned_dealloc, false);
}
static void *mtl_rpda_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_rpda_copy(s, sel); }
static void *mtl_rpd_d_tex(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->texture_obj : nullptr; }
static void  mtl_rpd_d_setTex(void *s, const char *sel, void *t) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->texture_obj = t; }
static void  mtl_rpd_d_setLoad(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->load_action = v; }
static void  mtl_rpd_d_setStore(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->store_action = v; }
static void  mtl_rpd_d_setClear(void *s, const char *sel, double v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->clear_depth = v; }

static void *mtl_rpsa_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlRenderPassStencilAttach> *>(s);
    if (w && !w->impl) w->impl = new MtlRenderPassStencilAttach();
    if (w) oh2_objc_set_dealloc(w, &mtl_rpsa_owned_dealloc);
    return s;
}
static void *mtl_rpsa_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlRenderPassStencilAttach>(s);
    auto *cp = src ? new MtlRenderPassStencilAttach(*src) : new MtlRenderPassStencilAttach();
    return alloc_mtl_wrap_managed("MTLRenderPassStencilAttachmentDescriptor", cp, &mtl_rpsa_owned_dealloc, false);
}
static void *mtl_rpsa_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_rpsa_copy(s, sel); }
static void *mtl_rpd_s_tex(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlRenderPassStencilAttach>(s); return a ? a->texture_obj : nullptr; }
static void  mtl_rpd_s_setTex(void *s, const char *sel, void *t) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassStencilAttach>(s)) a->texture_obj = t; }
static void  mtl_rpd_s_setLoad(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassStencilAttach>(s)) a->load_action = v; }
static void  mtl_rpd_s_setStore(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassStencilAttach>(s)) a->store_action = v; }
static void  mtl_rpd_s_setClear(void *s, const char *sel, uint32_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassStencilAttach>(s)) a->clear_stencil = v; }

/* MTLVertexDescriptor */
static void *mtl_vd_cls_desc(void *cls, const char *sel) {
    (void)cls; (void)sel;
    return alloc_mtl_wrap_managed("MTLVertexDescriptor", new MtlVertexDescriptorImpl(), &mtl_vd_dealloc, true);
}
static void *mtl_vd_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlVertexDescriptorImpl> *>(s);
    if (w && !w->impl) w->impl = new MtlVertexDescriptorImpl();
    if (w) oh2_objc_set_dealloc(w, &mtl_vd_dealloc);
    return s;
}
static void *mtl_vd_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlVertexDescriptorImpl>(s);
    auto *cp = src ? new MtlVertexDescriptorImpl(*src) : new MtlVertexDescriptorImpl();
    cp->attr_array_obj = nullptr;
    cp->layout_array_obj = nullptr;
    for (int i = 0; i < 32; ++i) {
        cp->attr_wraps[i] = nullptr;
        cp->layout_wraps[i] = nullptr;
    }
    return alloc_mtl_wrap_managed("MTLVertexDescriptor", cp, &mtl_vd_dealloc, false);
}
static void *mtl_vd_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_vd_copy(s, sel); }
static void *mtl_va_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlVertexAttrDesc> *>(s);
    if (w && !w->impl) w->impl = new MtlVertexAttrDesc();
    if (w) oh2_objc_set_dealloc(w, &mtl_va_owned_dealloc);
    return s;
}
static void *mtl_va_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlVertexAttrDesc>(s);
    auto *cp = src ? new MtlVertexAttrDesc(*src) : new MtlVertexAttrDesc();
    return alloc_mtl_wrap_managed("MTLVertexAttributeDescriptor", cp, &mtl_va_owned_dealloc, false);
}
static void *mtl_va_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_va_copy(s, sel); }
static void *mtl_vl_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlVertexLayoutDesc> *>(s);
    if (w && !w->impl) w->impl = new MtlVertexLayoutDesc();
    if (w) oh2_objc_set_dealloc(w, &mtl_vl_owned_dealloc);
    return s;
}
static void *mtl_vl_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlVertexLayoutDesc>(s);
    auto *cp = src ? new MtlVertexLayoutDesc(*src) : new MtlVertexLayoutDesc();
    return alloc_mtl_wrap_managed("MTLVertexBufferLayoutDescriptor", cp, &mtl_vl_owned_dealloc, false);
}
static void *mtl_vl_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_vl_copy(s, sel); }
static void *mtl_vd_attributes(void *s, const char *sel) {
    (void)sel;
    auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(s);
    if (!vd) return nullptr;
    if (!vd->attr_array_obj) {
        vd->attr_array_obj = alloc_mtl_wrap_managed(
            "MTLVertexAttributeDescriptorArray", new ArrayProxy{s}, &mtl_array_proxy_dealloc, false);
    }
    return vd->attr_array_obj;
}
static void *mtl_vd_layouts(void *s, const char *sel) {
    (void)sel;
    auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(s);
    if (!vd) return nullptr;
    if (!vd->layout_array_obj) {
        vd->layout_array_obj = alloc_mtl_wrap_managed(
            "MTLVertexBufferLayoutDescriptorArray", new ArrayProxy{s}, &mtl_array_proxy_dealloc, false);
    }
    return vd->layout_array_obj;
}
static void mtl_vd_reset(void *s, const char *sel) {
    (void)sel;
    if (auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(s)) {
        std::memset(vd->attrs, 0, sizeof(vd->attrs));
        for (int i = 0; i < 32; ++i) {
            vd->layouts[i].stride = 0;
            vd->layouts[i].step_function = 1;
            vd->layouts[i].step_rate = 1;
        }
    }
}
static uint64_t mtl_va_getFmt(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlVertexAttrDesc>(s); return a ? a->format : 0; }
static void mtl_va_setFmt(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlVertexAttrDesc>(s)) a->format = v; }
static uint64_t mtl_va_getOff(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlVertexAttrDesc>(s); return a ? a->offset : 0; }
static void mtl_va_setOff(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlVertexAttrDesc>(s)) a->offset = v; }
static uint64_t mtl_va_getBuf(void *s, const char *sel) { (void)sel; auto *a = unwrap_mtl<MtlVertexAttrDesc>(s); return a ? a->buffer_index : 0; }
static void mtl_va_setBuf(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlVertexAttrDesc>(s)) a->buffer_index = v; }
static uint64_t mtl_vl_getStride(void *s, const char *sel) { (void)sel; auto *l = unwrap_mtl<MtlVertexLayoutDesc>(s); return l ? l->stride : 0; }
static void mtl_vl_setStride(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *l = unwrap_mtl<MtlVertexLayoutDesc>(s)) l->stride = v; }
static uint64_t mtl_vl_getStepFn(void *s, const char *sel) { (void)sel; auto *l = unwrap_mtl<MtlVertexLayoutDesc>(s); return l ? l->step_function : 1; }
static void mtl_vl_setStepFn(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *l = unwrap_mtl<MtlVertexLayoutDesc>(s)) l->step_function = v; }
static uint64_t mtl_vl_getStepRate(void *s, const char *sel) { (void)sel; auto *l = unwrap_mtl<MtlVertexLayoutDesc>(s); return l ? l->step_rate : 1; }
static void mtl_vl_setStepRate(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *l = unwrap_mtl<MtlVertexLayoutDesc>(s)) l->step_rate = v; }

/* MTLRenderPipelineDescriptor & ColorAttachment */
static void *mtl_rpl_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlRenderPipelineDescImpl> *>(s);
    if (w && !w->impl) w->impl = new MtlRenderPipelineDescImpl();
    if (w) oh2_objc_set_dealloc(w, &mtl_rpl_dealloc);
    return s;
}
static void *mtl_rpl_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlRenderPipelineDescImpl>(s);
    auto *cp = src ? new MtlRenderPipelineDescImpl(*src) : new MtlRenderPipelineDescImpl();
    cp->color_array_obj = nullptr;
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) cp->color_wraps[i] = nullptr;
    return alloc_mtl_wrap_managed("MTLRenderPipelineDescriptor", cp, &mtl_rpl_dealloc, false);
}
static void *mtl_rpl_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_rpl_copy(s, sel); }
static void mtl_rpl_reset(void *s, const char *sel) {
    (void)sel;
    if (auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s)) {
        d->vertex_fn_obj = nullptr;
        d->fragment_fn_obj = nullptr;
        d->vertex_desc_obj = nullptr;
        d->depth_pixel_format = 0;
        d->stencil_pixel_format = 0;
        d->sample_count = 1;
        d->alpha_to_coverage = 0;
        d->label.clear();
        for (uint32_t i = 0; i < kMaxColorAttachments; ++i) {
            d->color[i] = MtlPipelineColorAttachDesc{};
        }
    }
}
static void *mtl_rpl_get_vs(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s); return d ? d->vertex_fn_obj : nullptr; }
static void  mtl_rpl_set_vs(void *s, const char *sel, void *f) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s)) d->vertex_fn_obj = f; }
static void *mtl_rpl_get_ps(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s); return d ? d->fragment_fn_obj : nullptr; }
static void  mtl_rpl_set_ps(void *s, const char *sel, void *f) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s)) d->fragment_fn_obj = f; }
static void *mtl_rpl_get_vd(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s); return d ? d->vertex_desc_obj : nullptr; }
static void  mtl_rpl_set_vd(void *s, const char *sel, void *vd) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s)) d->vertex_desc_obj = vd; }
static void  mtl_rpl_set_depth(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s)) d->depth_pixel_format = v; }
static void  mtl_rpl_set_stencil(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s)) d->stencil_pixel_format = v; }
static void  mtl_rpl_set_samples(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s)) d->sample_count = v; }
static uint8_t mtl_rpl_get_a2c(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s); return d ? d->alpha_to_coverage : 0; }
static void  mtl_rpl_set_a2c(void *s, const char *sel, uint8_t v) { (void)sel; if (auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s)) d->alpha_to_coverage = v ? 1 : 0; }
static void *mtl_rpl_colorAttachments(void *s, const char *sel) {
    (void)sel;
    auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s);
    if (!d) return nullptr;
    if (!d->color_array_obj) {
        d->color_array_obj = alloc_mtl_wrap_managed(
            "MTLRenderPipelineColorAttachmentDescriptorArray", new ArrayProxy{s}, &mtl_array_proxy_dealloc, false);
    }
    return d->color_array_obj;
}

static void *mtl_pca_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlPipelineColorAttachDesc> *>(s);
    if (w && !w->impl) w->impl = new MtlPipelineColorAttachDesc();
    if (w) oh2_objc_set_dealloc(w, &mtl_pca_owned_dealloc);
    return s;
}
static void *mtl_pca_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlPipelineColorAttachDesc>(s);
    auto *cp = src ? new MtlPipelineColorAttachDesc(*src) : new MtlPipelineColorAttachDesc();
    return alloc_mtl_wrap_managed("MTLRenderPipelineColorAttachmentDescriptor", cp, &mtl_pca_owned_dealloc, false);
}
static void *mtl_pca_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_pca_copy(s, sel); }
static uint64_t mtl_pca_get_fmt(void *s, const char *sel) { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->pixel_format : 0; }
static void mtl_pca_set_fmt(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->pixel_format = v; }
static void mtl_pca_set_blend(void *s, const char *sel, uint8_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->blending_enabled = v; }
static void mtl_pca_set_srgb(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->src_rgb = v; }
static void mtl_pca_set_drgb(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->dst_rgb = v; }
static void mtl_pca_set_rgbop(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->rgb_op = v; }
static void mtl_pca_set_sa(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->src_alpha = v; }
static void mtl_pca_set_da(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->dst_alpha = v; }
static void mtl_pca_set_aop(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->alpha_op = v; }
static void mtl_pca_set_wmask(void *s, const char *sel, uint64_t v) { (void)sel; if (auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s)) c->write_mask = v; }

/* MTLComputePipelineDescriptor */
struct MtlComputePipelineDescImpl {
    void *compute_fn_obj = nullptr;
    std::string label;
};
static void mtl_cpd_dealloc(void *self) {
    delete unwrap_mtl<MtlComputePipelineDescImpl>(self);
}
static void *mtl_cpd_init(void *s, const char *sel) {
    (void)sel;
    auto *w = static_cast<MtlObjWrap<MtlComputePipelineDescImpl> *>(s);
    if (w && !w->impl) w->impl = new MtlComputePipelineDescImpl();
    if (w) oh2_objc_set_dealloc(w, &mtl_cpd_dealloc);
    return s;
}
static void *mtl_cpd_copy(void *s, const char *sel) {
    (void)sel;
    auto *src = unwrap_mtl<MtlComputePipelineDescImpl>(s);
    auto *cp = src ? new MtlComputePipelineDescImpl(*src) : new MtlComputePipelineDescImpl();
    return alloc_mtl_wrap_managed("MTLComputePipelineDescriptor", cp, &mtl_cpd_dealloc, false);
}
static void *mtl_cpd_copyWithZone(void *s, const char *sel, void *z) { (void)z; return mtl_cpd_copy(s, sel); }
static void mtl_cpd_reset(void *s, const char *sel) {
    (void)sel;
    if (auto *d = unwrap_mtl<MtlComputePipelineDescImpl>(s)) {
        d->compute_fn_obj = nullptr;
        d->label.clear();
    }
}
static void *mtl_cpd_get_fn(void *s, const char *sel) { (void)sel; auto *d = unwrap_mtl<MtlComputePipelineDescImpl>(s); return d ? d->compute_fn_obj : nullptr; }
static void  mtl_cpd_set_fn(void *s, const char *sel, void *f) { (void)sel; if (auto *d = unwrap_mtl<MtlComputePipelineDescImpl>(s)) d->compute_fn_obj = f; }

/* Vulkan conversion helpers (Apple Metal SDK <Metal/MTLStageInputOutputDescriptor.h> / <Metal/MTLRenderPipeline.h>) */
static VkFormat map_vertex_format(uint64_t mtl_vfmt) {
    switch (mtl_vfmt) {
        case 1:  return VK_FORMAT_R8G8_UINT;                 /* UChar2 */
        case 2:  return VK_FORMAT_R8G8B8A8_UINT;             /* UChar3 (4-byte safe fallback) */
        case 3:  return VK_FORMAT_R8G8B8A8_UINT;             /* UChar4 */
        case 4:  return VK_FORMAT_R8G8_SINT;                 /* Char2 */
        case 5:  return VK_FORMAT_R8G8B8A8_SINT;             /* Char3 (4-byte safe fallback) */
        case 6:  return VK_FORMAT_R8G8B8A8_SINT;             /* Char4 */
        case 7:  return VK_FORMAT_R8G8_UNORM;                /* UChar2Normalized */
        case 8:  return VK_FORMAT_R8G8B8A8_UNORM;            /* UChar3Normalized (4-byte safe fallback) */
        case 9:  return VK_FORMAT_R8G8B8A8_UNORM;            /* UChar4Normalized */
        case 10: return VK_FORMAT_R8G8_SNORM;                /* Char2Normalized */
        case 11: return VK_FORMAT_R8G8B8A8_SNORM;            /* Char3Normalized (4-byte safe fallback) */
        case 12: return VK_FORMAT_R8G8B8A8_SNORM;            /* Char4Normalized */
        case 13: return VK_FORMAT_R16G16_UINT;               /* UShort2 */
        case 14: return VK_FORMAT_R16G16B16A16_UINT;         /* UShort3 */
        case 15: return VK_FORMAT_R16G16B16A16_UINT;         /* UShort4 */
        case 16: return VK_FORMAT_R16G16_SINT;               /* Short2 */
        case 17: return VK_FORMAT_R16G16B16A16_SINT;         /* Short3 */
        case 18: return VK_FORMAT_R16G16B16A16_SINT;         /* Short4 */
        case 19: return VK_FORMAT_R16G16_UNORM;              /* UShort2Normalized */
        case 20: return VK_FORMAT_R16G16B16A16_UNORM;        /* UShort3Normalized */
        case 21: return VK_FORMAT_R16G16B16A16_UNORM;        /* UShort4Normalized */
        case 22: return VK_FORMAT_R16G16_SNORM;              /* Short2Normalized */
        case 23: return VK_FORMAT_R16G16B16A16_SNORM;        /* Short3Normalized */
        case 24: return VK_FORMAT_R16G16B16A16_SNORM;        /* Short4Normalized */
        case 25: return VK_FORMAT_R16G16_SFLOAT;             /* Half2 */
        case 26: return VK_FORMAT_R16G16B16A16_SFLOAT;       /* Half3 (safe fallback) */
        case 27: return VK_FORMAT_R16G16B16A16_SFLOAT;       /* Half4 */
        case 28: return VK_FORMAT_R32_SFLOAT;                /* Float */
        case 29: return VK_FORMAT_R32G32_SFLOAT;             /* Float2 */
        case 30: return VK_FORMAT_R32G32B32_SFLOAT;          /* Float3 */
        case 31: return VK_FORMAT_R32G32B32A32_SFLOAT;       /* Float4 */
        case 32: return VK_FORMAT_R32_SINT;                  /* Int */
        case 33: return VK_FORMAT_R32G32_SINT;               /* Int2 */
        case 34: return VK_FORMAT_R32G32B32_SINT;            /* Int3 */
        case 35: return VK_FORMAT_R32G32B32A32_SINT;         /* Int4 */
        case 36: return VK_FORMAT_R32_UINT;                  /* UInt */
        case 37: return VK_FORMAT_R32G32_UINT;               /* UInt2 */
        case 38: return VK_FORMAT_R32G32B32_UINT;            /* UInt3 */
        case 39: return VK_FORMAT_R32G32B32A32_UINT;         /* UInt4 */
        case 40: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;  /* Int1010102Normalized */
        case 41: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;  /* UInt1010102Normalized */
        case 42: return VK_FORMAT_B8G8R8A8_UNORM;            /* UChar4Normalized_BGRA */
        case 45: return VK_FORMAT_R8_UINT;                   /* UChar */
        case 46: return VK_FORMAT_R8_SINT;                   /* Char */
        case 47: return VK_FORMAT_R8_UNORM;                  /* UCharNormalized */
        case 48: return VK_FORMAT_R8_SNORM;                  /* CharNormalized */
        case 49: return VK_FORMAT_R16_UINT;                  /* UShort */
        case 50: return VK_FORMAT_R16_SINT;                  /* Short */
        case 51: return VK_FORMAT_R16_UNORM;                 /* UShortNormalized */
        case 52: return VK_FORMAT_R16_SNORM;                 /* ShortNormalized */
        case 53: return VK_FORMAT_R16_SFLOAT;                /* Half */
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor map_blend_factor(uint64_t f) {
    switch (f) {
        case 0:  return VK_BLEND_FACTOR_ZERO;
        case 1:  return VK_BLEND_FACTOR_ONE;
        case 2:  return VK_BLEND_FACTOR_SRC_COLOR;
        case 3:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4:  return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6:  return VK_BLEND_FACTOR_DST_COLOR;
        case 7:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 8:  return VK_BLEND_FACTOR_DST_ALPHA;
        case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 11: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 12: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 16: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 17: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 18: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp map_blend_op(uint64_t op) {
    switch (op) {
        case 1:  return VK_BLEND_OP_SUBTRACT;
        case 2:  return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 3:  return VK_BLEND_OP_MIN;
        case 4:  return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

static VkStencilOp map_stencil_op(uint64_t op) {
    switch (op) {
        case 1: return VK_STENCIL_OP_ZERO;
        case 2: return VK_STENCIL_OP_REPLACE;
        case 3: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 4: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 5: return VK_STENCIL_OP_INVERT;
        case 6: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 7: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

static VkCompareOp map_cmp(uint64_t c) {
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

static void mtl_refl_dealloc(void *self) {
    delete unwrap_mtl<int>(self);
}

static void *mtl_device_newRenderPipelineState(void *self, const char *sel, void *desc_obj, uint64_t opts,
                                               void **refl, void **err) {
    (void)self; (void)sel; (void)opts;
    if (refl) *refl = alloc_mtl_wrap_managed("MTLRenderPipelineReflection", new int(1), &mtl_refl_dealloc, true);
    if (err) *err = nullptr;
    MtlVkState &st = get_state();
    if (!st.initialized || !desc_obj) return nullptr;

    auto *desc = unwrap_mtl<MtlRenderPipelineDescImpl>(desc_obj);
    if (!desc) return nullptr;

    auto *pso = new MtlRenderPipelineStateImpl();
    pso->desc = *desc;
    pso->desc.color_array_obj = nullptr;
    for (uint32_t i = 0; i < kMaxColorAttachments; ++i) pso->desc.color_wraps[i] = nullptr;
    if (desc->vertex_desc_obj) {
        auto *vd = unwrap_mtl<MtlVertexDescriptorImpl>(desc->vertex_desc_obj);
        if (vd) {
            pso->vtx_desc_copy = *vd;
            pso->vtx_desc_copy.attr_array_obj = nullptr;
            pso->vtx_desc_copy.layout_array_obj = nullptr;
            for (int i = 0; i < 32; ++i) {
                pso->vtx_desc_copy.attr_wraps[i] = nullptr;
                pso->vtx_desc_copy.layout_wraps[i] = nullptr;
            }
            for (int i = 0; i < 31; ++i) {
                if (vd->layouts[i].stride > 0) {
                    pso->vertex_buffer_mask |= (1u << i);
                }
            }
        }
    }

    if (desc->vertex_fn_obj) {
        auto *fn = unwrap_mtl<MtlFunctionImpl>(desc->vertex_fn_obj);
        if (fn) {
            pso->vs = fn->entry;
            pso->vs_module = fn->module;
        }
    }
    if (desc->fragment_fn_obj) {
        auto *fn = unwrap_mtl<MtlFunctionImpl>(desc->fragment_fn_obj);
        if (fn) {
            pso->ps = fn->entry;
            pso->ps_module = fn->module;
        }
    }

    /* Set 0 (VS) & Set 1 (PS) layouts strictly matching M4 spirv_cache.h ABI */
    pso->set0_layout = create_stage_descriptor_layout(pso->vs, VK_SHADER_STAGE_VERTEX_BIT);
    pso->set1_layout = create_stage_descriptor_layout(pso->ps, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkDescriptorSetLayout set_layouts[2] = {pso->set0_layout, pso->set1_layout};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = set_layouts;
    if (vkCreatePipelineLayout(st.vk_ctx.device(), &plci, nullptr, &pso->pipeline_layout) != VK_SUCCESS) {
        MTLVK_LOGE("vkCreatePipelineLayout (graphics) failed");
        return nullptr;
    }

    return alloc_mtl_wrap("MTLRenderPipelineState", pso);
}

static void *mtl_device_newRenderPipelineStateErr(void *self, const char *sel, void *desc_obj, void **err) {
    return mtl_device_newRenderPipelineState(self, sel, desc_obj, 0, nullptr, err);
}

static void *mtl_device_newComputePipelineWithFn(void *self, const char *sel, void *fn_obj, uint64_t opts,
                                                  void **refl, void **err) {
    (void)self; (void)sel; (void)opts;
    if (refl) *refl = alloc_mtl_wrap_managed("MTLComputePipelineReflection", new int(1), &mtl_refl_dealloc, true);
    if (err) *err = nullptr;
    MtlVkState &st = get_state();
    if (!st.initialized || !fn_obj) return nullptr;

    auto *fn = unwrap_mtl<MtlFunctionImpl>(fn_obj);
    if (!fn || fn->module == VK_NULL_HANDLE) {
        if (err) *err = create_ns_error("Missing compute VkShaderModule");
        return nullptr;
    }

    auto *cpso = new MtlComputePipelineStateImpl();
    cpso->cs = fn->entry;
    cpso->cs_module = fn->module;
    cpso->set0_layout = create_stage_descriptor_layout(cpso->cs, VK_SHADER_STAGE_COMPUTE_BIT);

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &cpso->set0_layout;
    if (vkCreatePipelineLayout(st.vk_ctx.device(), &plci, nullptr, &cpso->pipeline_layout) != VK_SUCCESS) {
        return nullptr;
    }

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = cpso->cs_module;
    cpci.stage.pName = (cpso->cs && cpso->cs->spirv_entry_point) ? cpso->cs->spirv_entry_point : "main";
    cpci.layout = cpso->pipeline_layout;

    if (vkCreateComputePipelines(st.vk_ctx.device(), st.pipeline_cache, 1, &cpci, nullptr, &cpso->pipeline) != VK_SUCCESS) {
        MTLVK_LOGE("vkCreateComputePipelines failed for '%s'", cpso->cs && cpso->cs->entry_name ? cpso->cs->entry_name : "?");
        return nullptr;
    }
    st.compute_pipelines_created.fetch_add(1, std::memory_order_relaxed);
    return alloc_mtl_wrap("MTLComputePipelineState", cpso);
}

static void *mtl_device_newComputePipelineFnErr(void *self, const char *sel, void *fn_obj, void **err) {
    return mtl_device_newComputePipelineWithFn(self, sel, fn_obj, 0, nullptr, err);
}

static void *mtl_device_newComputePipelineDesc(void *self, const char *sel, void *desc_obj, uint64_t opts,
                                              void **refl, void **err) {
    auto *d = unwrap_mtl<MtlComputePipelineDescImpl>(desc_obj);
    return mtl_device_newComputePipelineWithFn(self, sel, d ? d->compute_fn_obj : nullptr, opts, refl, err);
}

static uint64_t mtl_cpso_maxThreads(void *s, const char *sel) {
    (void)sel;
    auto *c = unwrap_mtl<MtlComputePipelineStateImpl>(s);
    return c ? c->max_threads_per_tg : 512ULL;
}
static uint64_t mtl_cpso_execWidth(void *s, const char *sel) {
    (void)sel;
    auto *c = unwrap_mtl<MtlComputePipelineStateImpl>(s);
    return c ? c->thread_exec_width : 32ULL;
}

} // namespace

VkDescriptorSetLayout create_stage_descriptor_layout(const SpirvShaderEntry *entry, VkShaderStageFlags stage_flag) {
    MtlVkState &st = get_state();
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    if (entry && entry->bindings && entry->binding_count > 0) {
        bindings.reserve(entry->binding_count);
        for (uint16_t i = 0; i < entry->binding_count; ++i) {
            const SpirvBindingDesc &b = entry->bindings[i];
            VkDescriptorSetLayoutBinding vk_b{};
            vk_b.binding = b.binding;
            vk_b.descriptorCount = 1;
            vk_b.stageFlags = stage_flag;
            switch (b.descriptor_type) {
                case SPIRV_VK_DESC_SAMPLER:        vk_b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; break;
                case SPIRV_VK_DESC_SAMPLED_IMAGE:  vk_b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; break;
                case SPIRV_VK_DESC_STORAGE_IMAGE:  vk_b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
                case SPIRV_VK_DESC_STORAGE_BUFFER: vk_b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
                default:                           vk_b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            }
            bindings.push_back(vk_b);
        }
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.empty() ? nullptr : bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(st.vk_ctx.device(), &info, nullptr, &layout);
    return layout;
}

VkRenderPass get_or_create_render_pass(const VkFormat *color_fmts, uint32_t color_count,
                                       const uint64_t *color_loads, const uint64_t *color_stores,
                                       VkFormat depth_fmt, uint64_t depth_load, uint64_t depth_store,
                                       bool for_compat) {
    MtlVkState &st = get_state();
    uint64_t key = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) { key ^= v; key *= 1099511628211ULL; };
    mix(color_count);
    for (uint32_t i = 0; i < color_count; ++i) {
        mix((uint64_t)color_fmts[i]);
        mix(for_compat ? 0 : color_loads[i]);
        mix(for_compat ? 1 : color_stores[i]);
    }
    mix((uint64_t)depth_fmt);
    mix(for_compat ? 0 : depth_load);
    mix(for_compat ? 0 : depth_store);

    {
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.render_pass_cache.find(key);
        if (it != st.render_pass_cache.end()) return it->second;
    }

    std::vector<VkAttachmentDescription> atts;
    std::vector<VkAttachmentReference> color_refs;
    for (uint32_t i = 0; i < color_count; ++i) {
        VkAttachmentDescription ad{};
        ad.format = color_fmts[i];
        ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = (color_loads[i] == kMTLLoadActionClear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                    (color_loads[i] == kMTLLoadActionLoad)  ? VK_ATTACHMENT_LOAD_OP_LOAD :
                                                              VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.storeOp = (color_stores[i] == kMTLStoreActionDontCare) ? VK_ATTACHMENT_STORE_OP_DONT_CARE :
                                                                    VK_ATTACHMENT_STORE_OP_STORE;
        ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_refs.push_back({static_cast<uint32_t>(atts.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        atts.push_back(ad);
    }

    VkAttachmentReference depth_ref{};
    bool has_depth = (depth_fmt != VK_FORMAT_UNDEFINED);
    if (has_depth) {
        VkAttachmentDescription dd{};
        dd.format = depth_fmt;
        dd.samples = VK_SAMPLE_COUNT_1_BIT;
        dd.loadOp = (depth_load == kMTLLoadActionClear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                    (depth_load == kMTLLoadActionLoad)  ? VK_ATTACHMENT_LOAD_OP_LOAD :
                                                          VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        dd.storeOp = (depth_store == kMTLStoreActionDontCare) ? VK_ATTACHMENT_STORE_OP_DONT_CARE :
                                                              VK_ATTACHMENT_STORE_OP_STORE;
        dd.stencilLoadOp = dd.loadOp;
        dd.stencilStoreOp = dd.storeOp;
        dd.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dd.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref = {static_cast<uint32_t>(atts.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        atts.push_back(dd);
    }

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = static_cast<uint32_t>(color_refs.size());
    sub.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
    sub.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = static_cast<uint32_t>(atts.size());
    rpci.pAttachments = atts.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(st.vk_ctx.device(), &rpci, nullptr, &rp) != VK_SUCCESS) {
        MTLVK_LOGE("vkCreateRenderPass failed");
        return VK_NULL_HANDLE;
    }

    std::lock_guard<std::mutex> lk(st.mutex);
    st.render_pass_cache[key] = rp;
    return rp;
}

VkPipeline get_or_create_specialized_graphics_pipeline(MtlRenderPipelineStateImpl *pso,
                                                         MtlDepthStencilStateImpl *dss,
                                                         uint64_t primitive_type,
                                                         uint64_t cull_mode,
                                                         uint64_t front_winding,
                                                         uint64_t fill_mode,
                                                         const VkFormat *color_fmts,
                                                         uint32_t color_count,
                                                         VkFormat depth_fmt,
                                                         VkFormat stencil_fmt,
                                                         VkRenderPass compat_rp) {
    MtlVkState &st = get_state();
    if (!pso || pso->vs_module == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    uint64_t key = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) { key ^= v; key *= 1099511628211ULL; };
    mix(primitive_type);
    mix(cull_mode);
    mix(front_winding);
    mix(fill_mode);
    mix(color_count);
    for (uint32_t i = 0; i < color_count; ++i) mix((uint64_t)color_fmts[i]);
    mix((uint64_t)depth_fmt);
    mix((uint64_t)stencil_fmt);
    mix((uint64_t)(uintptr_t)compat_rp);
    if (dss) {
        mix(dss->desc.depth_compare_fn);
        mix(dss->desc.depth_write_enabled);
        mix(dss->desc.front.compare_fn);
        mix(dss->desc.front.stencil_fail_op);
        mix(dss->desc.front.depth_fail_op);
        mix(dss->desc.front.depth_stencil_pass_op);
        mix(dss->desc.front.read_mask);
        mix(dss->desc.front.write_mask);
        mix(dss->desc.back.compare_fn);
        mix(dss->desc.back.stencil_fail_op);
        mix(dss->desc.back.depth_fail_op);
        mix(dss->desc.back.depth_stencil_pass_op);
        mix(dss->desc.back.read_mask);
        mix(dss->desc.back.write_mask);
    }

    auto it = pso->specialized_pipelines.find(key);
    if (it != pso->specialized_pipelines.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    uint32_t stage_count = 0;
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stage_count].module = pso->vs_module;
    stages[stage_count].pName = (pso->vs && pso->vs->spirv_entry_point) ? pso->vs->spirv_entry_point : "main";
    stage_count++;

    if (pso->ps_module != VK_NULL_HANDLE) {
        stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[stage_count].module = pso->ps_module;
        stages[stage_count].pName = (pso->ps && pso->ps->spirv_entry_point) ? pso->ps->spirv_entry_point : "main";
        stage_count++;
    }

    /* Vertex Input strictly filtered to attributes used in VS SPIR-V (avoid validation mismatches) */
    std::vector<VkVertexInputBindingDescription> vbindings;
    std::vector<VkVertexInputAttributeDescription> vattrs;
    bool used_binding[32]{};
    for (int loc = 0; loc < 31; ++loc) {
        const auto &a = pso->vtx_desc_copy.attrs[loc];
        if (a.format == 0) continue;
        bool declared_in_vs = true;
        if (pso->vs && pso->vs->vertex_attr_count > 0) {
            declared_in_vs = false;
            for (uint16_t k = 0; k < pso->vs->vertex_attr_count; ++k) {
                if (pso->vs->vertex_attrs[k].location == (uint8_t)loc) {
                    declared_in_vs = true;
                    break;
                }
            }
        }
        if (!declared_in_vs) continue;
        if (a.buffer_index >= 31 || pso->vtx_desc_copy.layouts[a.buffer_index].stride == 0) continue;

        VkVertexInputAttributeDescription vad{};
        vad.location = (uint32_t)loc;
        vad.binding = (uint32_t)a.buffer_index;
        vad.format = map_vertex_format(a.format);
        vad.offset = (uint32_t)a.offset;
        vattrs.push_back(vad);

        if (!used_binding[a.buffer_index]) {
            used_binding[a.buffer_index] = true;
            VkVertexInputBindingDescription vbd{};
            vbd.binding = (uint32_t)a.buffer_index;
            vbd.stride = (uint32_t)pso->vtx_desc_copy.layouts[a.buffer_index].stride;
            vbd.inputRate = (pso->vtx_desc_copy.layouts[a.buffer_index].step_function == 2)
                                ? VK_VERTEX_INPUT_RATE_INSTANCE
                                : VK_VERTEX_INPUT_RATE_VERTEX;
            vbindings.push_back(vbd);
        }
    }

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t)vbindings.size();
    vi.pVertexBindingDescriptions = vbindings.empty() ? nullptr : vbindings.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)vattrs.size();
    vi.pVertexAttributeDescriptions = vattrs.empty() ? nullptr : vattrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    switch (primitive_type) {
        case kMTLPrimitivePoint:         ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case kMTLPrimitiveLine:          ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case kMTLPrimitiveLineStrip:     ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case kMTLPrimitiveTriangleStrip: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        default:                         ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    }

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = (fill_mode == 1 && st.vk_ctx.caps().supported_features.fillModeNonSolid)
                         ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = (cull_mode == kMTLCullFront) ? VK_CULL_MODE_FRONT_BIT :
                  (cull_mode == kMTLCullBack)  ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    /* VK_KHR_maintenance1 negative viewport height maps Metal NDC (+Y up) to Vulkan framebuffer (+Y down)
       identically to Metal, preserving screen-space CW/CCW winding. */
    rs.frontFace = (front_winding == kMTLWindingCW) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = VK_TRUE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.alphaToCoverageEnable = pso->desc.alpha_to_coverage ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (dss && depth_fmt != VK_FORMAT_UNDEFINED) {
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = dss->desc.depth_write_enabled ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = map_cmp(dss->desc.depth_compare_fn);
        if (stencil_fmt != VK_FORMAT_UNDEFINED) {
            ds.stencilTestEnable = VK_TRUE;
            ds.front.compareOp = map_cmp(dss->desc.front.compare_fn);
            ds.front.failOp = map_stencil_op(dss->desc.front.stencil_fail_op);
            ds.front.depthFailOp = map_stencil_op(dss->desc.front.depth_fail_op);
            ds.front.passOp = map_stencil_op(dss->desc.front.depth_stencil_pass_op);
            ds.front.compareMask = dss->desc.front.read_mask;
            ds.front.writeMask = dss->desc.front.write_mask;
            ds.back.compareOp = map_cmp(dss->desc.back.compare_fn);
            ds.back.failOp = map_stencil_op(dss->desc.back.stencil_fail_op);
            ds.back.depthFailOp = map_stencil_op(dss->desc.back.depth_fail_op);
            ds.back.passOp = map_stencil_op(dss->desc.back.depth_stencil_pass_op);
            ds.back.compareMask = dss->desc.back.read_mask;
            ds.back.writeMask = dss->desc.back.write_mask;
        }
    }

    std::vector<VkPipelineColorBlendAttachmentState> cbs(color_count);
    bool any_blend_enabled = false;
    for (uint32_t i = 0; i < color_count; ++i) {
        const auto &pc = pso->desc.color[i];
        cbs[i].blendEnable = pc.blending_enabled ? VK_TRUE : VK_FALSE;
        if (pc.blending_enabled) any_blend_enabled = true;
        cbs[i].srcColorBlendFactor = map_blend_factor(pc.src_rgb);
        cbs[i].dstColorBlendFactor = map_blend_factor(pc.dst_rgb);
        cbs[i].colorBlendOp = map_blend_op(pc.rgb_op);
        cbs[i].srcAlphaBlendFactor = map_blend_factor(pc.src_alpha);
        cbs[i].dstAlphaBlendFactor = map_blend_factor(pc.dst_alpha);
        cbs[i].alphaBlendOp = map_blend_op(pc.alpha_op);
        /* Apple MTLColorWriteMask: Red=0x8, Green=0x4, Blue=0x2, Alpha=0x1 */
        uint32_t wmask = 0;
        if (pc.write_mask & 0x8ULL) wmask |= VK_COLOR_COMPONENT_R_BIT;
        if (pc.write_mask & 0x4ULL) wmask |= VK_COLOR_COMPONENT_G_BIT;
        if (pc.write_mask & 0x2ULL) wmask |= VK_COLOR_COMPONENT_B_BIT;
        if (pc.write_mask & 0x1ULL) wmask |= VK_COLOR_COMPONENT_A_BIT;
        if (pc.write_mask == 0) wmask = 0;
        cbs[i].colorWriteMask = (wmask == 0 && !pc.blending_enabled && pc.pixel_format == 0)
                                    ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
                                    : wmask;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = color_count;
    cb.pAttachments = cbs.empty() ? nullptr : cbs.data();

    VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = sizeof(dyn_states) / sizeof(dyn_states[0]);
    dyn.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfoKHR dyn_render{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    dyn_render.colorAttachmentCount = color_count;
    dyn_render.pColorAttachmentFormats = color_count ? color_fmts : nullptr;
    dyn_render.depthAttachmentFormat = depth_fmt;
    dyn_render.stencilAttachmentFormat = stencil_fmt;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = stage_count;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = pso->pipeline_layout;

    if (st.vk_ctx.caps().dynamic_rendering && compat_rp == VK_NULL_HANDLE) {
        gpci.pNext = &dyn_render;
        gpci.renderPass = VK_NULL_HANDLE;
    } else {
        gpci.renderPass = compat_rp;
        gpci.subpass = 0;
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(st.vk_ctx.device(), st.pipeline_cache, 1, &gpci, nullptr, &pipeline) != VK_SUCCESS) {
        MTLVK_LOGE("vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    pso->specialized_pipelines[key] = pipeline;
    st.graphics_pipelines_created.fetch_add(1, std::memory_order_relaxed);
    if (any_blend_enabled) {
        st.blended_pipelines_created.fetch_add(1, std::memory_order_relaxed);
    }
    return pipeline;
}

void register_pipeline_objc_methods() {
    oh2_objc_ensure_class("MTLRenderPipelineState", "NSObject");
    oh2_objc_ensure_class("MTLComputePipelineState", "NSObject");
    oh2_objc_ensure_class("MTLRenderPassColorAttachmentDescriptorArray", "NSObject");
    oh2_objc_ensure_class("MTLRenderPipelineColorAttachmentDescriptorArray", "NSObject");
    oh2_objc_ensure_class("MTLVertexAttributeDescriptorArray", "NSObject");
    oh2_objc_ensure_class("MTLVertexBufferLayoutDescriptorArray", "NSObject");

    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptorArray", "objectAtIndexedSubscript:", (void *)&mtl_rp_color_arr_subscript);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptorArray", "setObject:atIndexedSubscript:", (void *)&mtl_rp_color_arr_set);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptorArray", "objectAtIndexedSubscript:", (void *)&mtl_rpd_color_arr_subscript);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptorArray", "setObject:atIndexedSubscript:", (void *)&mtl_rpd_color_arr_set);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptorArray", "objectAtIndexedSubscript:", (void *)&mtl_vd_attr_arr_subscript);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptorArray", "setObject:atIndexedSubscript:", (void *)&mtl_vd_attr_arr_set);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptorArray", "objectAtIndexedSubscript:", (void *)&mtl_vd_layout_arr_subscript);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptorArray", "setObject:atIndexedSubscript:", (void *)&mtl_vd_layout_arr_set);

    oh2_objc_register_class_method("MTLRenderPassDescriptor",    "renderPassDescriptor", (void *)&mtl_rpd_cls_desc);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "init",                 (void *)&mtl_rpd_init);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "copy",                 (void *)&mtl_rpd_copy);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "copyWithZone:",        (void *)&mtl_rpd_copyWithZone);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "colorAttachments",     (void *)&mtl_rpd_colorAttachments);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "depthAttachment",      (void *)&mtl_rpd_depthAttachment);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setDepthAttachment:",  (void *)&mtl_rpd_setDepthAttachment);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "stencilAttachment",    (void *)&mtl_rpd_stencilAttachment);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setStencilAttachment:", (void *)&mtl_rpd_setStencilAttachment);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setRenderTargetWidth:", (void *)&mtl_rpd_setRTW);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setRenderTargetHeight:", (void *)&mtl_rpd_setRTH);
    static auto mtl_rpd_getRTW   = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s); return d ? d->rt_width : 0ULL; };
    static auto mtl_rpd_getRTH   = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlRenderPassDescImpl>(s); return d ? d->rt_height : 0ULL; };
    static auto mtl_pip_zero_u64 = +[](void *s, const char *sel) -> uint64_t { (void)s; (void)sel; return 0ULL; };
    static auto mtl_pip_one_u64  = +[](void *s, const char *sel) -> uint64_t { (void)s; (void)sel; return 1ULL; };
    static auto mtl_pip_one_u8   = +[](void *s, const char *sel) -> uint8_t  { (void)s; (void)sel; return 1; };
    static auto mtl_pip_zero_u8  = +[](void *s, const char *sel) -> uint8_t  { (void)s; (void)sel; return 0; };
    static auto mtl_pip_zero_ptr = +[](void *s, const char *sel) -> void *   { (void)s; (void)sel; return nullptr; };
    static auto mtl_pip_noop_u64 = +[](void *s, const char *sel, uint64_t v) { (void)s; (void)sel; (void)v; };
    static auto mtl_pip_noop_u8  = +[](void *s, const char *sel, uint8_t v)  { (void)s; (void)sel; (void)v; };
    static auto mtl_pip_noop_ptr = +[](void *s, const char *sel, void *v)    { (void)s; (void)sel; (void)v; };
    static auto mtl_pip_noop_ptr_u64 = +[](void *s, const char *sel, void *p, uint64_t c) { (void)s; (void)sel; (void)p; (void)c; };
    static auto mtl_pip_device   = +[](void *s, const char *sel) -> void *   { (void)s; (void)sel; return get_state().device_obj; };
    static auto mtl_pip_label    = +[](void *s, const char *sel) -> void *   { (void)s; (void)sel; return oh2_objc_create_string("MTLPipeline"); };
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "renderTargetWidth",          (void *)mtl_rpd_getRTW);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "renderTargetHeight",         (void *)mtl_rpd_getRTH);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "renderTargetArrayLength",    (void *)mtl_pip_one_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setRenderTargetArrayLength:",(void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "defaultRasterSampleCount",   (void *)mtl_pip_one_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setDefaultRasterSampleCount:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "visibilityResultBuffer",     (void *)mtl_pip_zero_ptr);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setVisibilityResultBuffer:", (void *)mtl_pip_noop_ptr);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "imageblockSampleLength",     (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setImageblockSampleLength:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "threadgroupMemoryLength",    (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setThreadgroupMemoryLength:",(void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "tileWidth",                  (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setTileWidth:",              (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "tileHeight",                 (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setTileHeight:",             (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "setSamplePositions:count:",  (void *)mtl_pip_noop_ptr_u64);
    oh2_objc_register_instance_method("MTLRenderPassDescriptor", "getSamplePositions:count:",  (void *)mtl_pip_noop_ptr_u64);

    static auto mtl_rpc_getLvl    = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->level : 0; };
    static auto mtl_rpc_getSlc    = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->slice : 0; };
    static auto mtl_rpc_getResLvl = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->resolve_level : 0; };
    static auto mtl_rpc_setResLvl = +[](void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->resolve_level = v; };
    static auto mtl_rpc_getResSlc = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s); return a ? a->resolve_slice : 0; };
    static auto mtl_rpc_setResSlc = +[](void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassColorAttach>(s)) a->resolve_slice = v; };
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "level",               (void *)mtl_rpc_getLvl);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "slice",               (void *)mtl_rpc_getSlc);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "depthPlane",          (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setDepthPlane:",      (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "resolveLevel",        (void *)mtl_rpc_getResLvl);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "resolvelevel",        (void *)mtl_rpc_getResLvl);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setResolveLevel:",    (void *)mtl_rpc_setResLvl);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "resolveSlice",        (void *)mtl_rpc_getResSlc);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "resolveslice",        (void *)mtl_rpc_getResSlc);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setResolveSlice:",    (void *)mtl_rpc_setResSlc);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "resolveDepthPlane",   (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setResolveDepthPlane:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "storeActionOptions",  (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setStoreActionOptions:", (void *)mtl_pip_noop_u64);

    static auto mtl_rpd_d_load      = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->load_action : 0; };
    static auto mtl_rpd_d_store     = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->store_action : 0; };
    static auto mtl_rpd_d_clr       = +[](void *s, const char *sel) -> double   { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->clear_depth : 1.0; };
    static auto mtl_rpd_d_resTex    = +[](void *s, const char *sel) -> void *   { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->resolve_texture_obj : nullptr; };
    static auto mtl_rpd_d_setResTex = +[](void *s, const char *sel, void *t)    { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->resolve_texture_obj = t; };
    static auto mtl_rpd_d_getLvl    = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->level : 0; };
    static auto mtl_rpd_d_setLvl    = +[](void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->level = v; };
    static auto mtl_rpd_d_getSlc    = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->slice : 0; };
    static auto mtl_rpd_d_setSlc    = +[](void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->slice = v; };
    static auto mtl_rpd_d_getResLvl = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->resolve_level : 0; };
    static auto mtl_rpd_d_setResLvl = +[](void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->resolve_level = v; };
    static auto mtl_rpd_d_getResSlc = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s); return a ? a->resolve_slice : 0; };
    static auto mtl_rpd_d_setResSlc = +[](void *s, const char *sel, uint64_t v) { (void)sel; if (auto *a = unwrap_mtl<MtlRenderPassDepthAttach>(s)) a->resolve_slice = v; };
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "loadAction",         (void *)mtl_rpd_d_load);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "storeAction",        (void *)mtl_rpd_d_store);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "clearDepth",         (void *)mtl_rpd_d_clr);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "depthResolveFilter", (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setDepthResolveFilter:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "resolveTexture",     (void *)mtl_rpd_d_resTex);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setResolveTexture:", (void *)mtl_rpd_d_setResTex);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "level",              (void *)mtl_rpd_d_getLvl);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setLevel:",          (void *)mtl_rpd_d_setLvl);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "slice",              (void *)mtl_rpd_d_getSlc);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setSlice:",          (void *)mtl_rpd_d_setSlc);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "resolveLevel",       (void *)mtl_rpd_d_getResLvl);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "resolvelevel",       (void *)mtl_rpd_d_getResLvl);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setResolveLevel:",   (void *)mtl_rpd_d_setResLvl);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "resolveSlice",       (void *)mtl_rpd_d_getResSlc);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "resolveslice",       (void *)mtl_rpd_d_getResSlc);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setResolveSlice:",   (void *)mtl_rpd_d_setResSlc);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "depthPlane",         (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setDepthPlane:",     (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "resolveDepthPlane",  (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setResolveDepthPlane:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "storeActionOptions", (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setStoreActionOptions:", (void *)mtl_pip_noop_u64);

    static auto mtl_rpd_s_load  = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassStencilAttach>(s); return a ? a->load_action : 0; };
    static auto mtl_rpd_s_store = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassStencilAttach>(s); return a ? a->store_action : 0; };
    static auto mtl_rpd_s_clr   = +[](void *s, const char *sel) -> uint32_t { (void)sel; auto *a = unwrap_mtl<MtlRenderPassStencilAttach>(s); return a ? a->clear_stencil : 0; };
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "loadAction",         (void *)mtl_rpd_s_load);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "storeAction",        (void *)mtl_rpd_s_store);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "clearStencil",       (void *)mtl_rpd_s_clr);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "resolveTexture",     (void *)mtl_pip_zero_ptr);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setResolveTexture:", (void *)mtl_pip_noop_ptr);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "level",              (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setLevel:",          (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "slice",              (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setSlice:",          (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "resolveLevel",       (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "resolvelevel",       (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setResolveLevel:",   (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "resolveSlice",       (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "resolveslice",       (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setResolveSlice:",   (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "depthPlane",         (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setDepthPlane:",     (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "resolveDepthPlane",  (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setResolveDepthPlane:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "storeActionOptions", (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setStoreActionOptions:", (void *)mtl_pip_noop_u64);

    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "init",               (void *)&mtl_rpc_init);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "copy",               (void *)&mtl_rpc_copy);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "copyWithZone:",      (void *)&mtl_rpc_copyWithZone);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "texture",            (void *)&mtl_rpc_tex);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setTexture:",        (void *)&mtl_rpc_setTex);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "resolveTexture",     (void *)&mtl_rpc_resTex);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setResolveTexture:", (void *)&mtl_rpc_setResTex);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setLevel:",          (void *)&mtl_rpc_setLvl);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setSlice:",          (void *)&mtl_rpc_setSlc);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "loadAction",         (void *)&mtl_rpc_load);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setLoadAction:",     (void *)&mtl_rpc_setLoad);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "storeAction",        (void *)&mtl_rpc_store);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setStoreAction:",    (void *)&mtl_rpc_setStore);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "clearColor",         (void *)&mtl_rpc_clear);
    oh2_objc_register_instance_method("MTLRenderPassColorAttachmentDescriptor", "setClearColor:",     (void *)&mtl_rpc_setClear);

    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "init",               (void *)&mtl_rpda_init);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "copy",               (void *)&mtl_rpda_copy);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "copyWithZone:",      (void *)&mtl_rpda_copyWithZone);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "texture",            (void *)&mtl_rpd_d_tex);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setTexture:",        (void *)&mtl_rpd_d_setTex);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setLoadAction:",     (void *)&mtl_rpd_d_setLoad);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setStoreAction:",    (void *)&mtl_rpd_d_setStore);
    oh2_objc_register_instance_method("MTLRenderPassDepthAttachmentDescriptor", "setClearDepth:",     (void *)&mtl_rpd_d_setClear);

    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "init",             (void *)&mtl_rpsa_init);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "copy",             (void *)&mtl_rpsa_copy);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "copyWithZone:",    (void *)&mtl_rpsa_copyWithZone);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "texture",          (void *)&mtl_rpd_s_tex);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setTexture:",      (void *)&mtl_rpd_s_setTex);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setLoadAction:",   (void *)&mtl_rpd_s_setLoad);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setStoreAction:",  (void *)&mtl_rpd_s_setStore);
    oh2_objc_register_instance_method("MTLRenderPassStencilAttachmentDescriptor", "setClearStencil:", (void *)&mtl_rpd_s_setClear);

    oh2_objc_register_class_method("MTLVertexDescriptor",    "vertexDescriptor", (void *)&mtl_vd_cls_desc);
    oh2_objc_register_instance_method("MTLVertexDescriptor", "init",             (void *)&mtl_vd_init);
    oh2_objc_register_instance_method("MTLVertexDescriptor", "copy",             (void *)&mtl_vd_copy);
    oh2_objc_register_instance_method("MTLVertexDescriptor", "copyWithZone:",    (void *)&mtl_vd_copyWithZone);
    oh2_objc_register_instance_method("MTLVertexDescriptor", "reset",            (void *)&mtl_vd_reset);
    oh2_objc_register_instance_method("MTLVertexDescriptor", "attributes",       (void *)&mtl_vd_attributes);
    oh2_objc_register_instance_method("MTLVertexDescriptor", "layouts",          (void *)&mtl_vd_layouts);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "init",             (void *)&mtl_va_init);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "copy",             (void *)&mtl_va_copy);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "copyWithZone:",    (void *)&mtl_va_copyWithZone);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "format",           (void *)&mtl_va_getFmt);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "setFormat:",       (void *)&mtl_va_setFmt);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "offset",           (void *)&mtl_va_getOff);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "setOffset:",       (void *)&mtl_va_setOff);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "bufferIndex",      (void *)&mtl_va_getBuf);
    oh2_objc_register_instance_method("MTLVertexAttributeDescriptor", "setBufferIndex:",  (void *)&mtl_va_setBuf);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "init",          (void *)&mtl_vl_init);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "copy",          (void *)&mtl_vl_copy);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "copyWithZone:", (void *)&mtl_vl_copyWithZone);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "stride",        (void *)&mtl_vl_getStride);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "setStride:",    (void *)&mtl_vl_setStride);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "stepFunction",  (void *)&mtl_vl_getStepFn);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "setStepFunction:", (void *)&mtl_vl_setStepFn);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "stepRate",      (void *)&mtl_vl_getStepRate);
    oh2_objc_register_instance_method("MTLVertexBufferLayoutDescriptor", "setStepRate:",  (void *)&mtl_vl_setStepRate);

    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "init",                   (void *)&mtl_rpl_init);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "copy",                   (void *)&mtl_rpl_copy);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "copyWithZone:",          (void *)&mtl_rpl_copyWithZone);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "vertexFunction",         (void *)&mtl_rpl_get_vs);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setVertexFunction:",     (void *)&mtl_rpl_set_vs);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "fragmentFunction",       (void *)&mtl_rpl_get_ps);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setFragmentFunction:",   (void *)&mtl_rpl_set_ps);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "vertexDescriptor",       (void *)&mtl_rpl_get_vd);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setVertexDescriptor:",   (void *)&mtl_rpl_set_vd);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "colorAttachments",       (void *)&mtl_rpl_colorAttachments);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setDepthAttachmentPixelFormat:", (void *)&mtl_rpl_set_depth);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setStencilAttachmentPixelFormat:", (void *)&mtl_rpl_set_stencil);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setSampleCount:",        (void *)&mtl_rpl_set_samples);
    static auto mtl_rpl_get_depth   = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s); return d ? d->depth_pixel_format : 0; };
    static auto mtl_rpl_get_stencil = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s); return d ? d->stencil_pixel_format : 0; };
    static auto mtl_rpl_get_samp    = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *d = unwrap_mtl<MtlRenderPipelineDescImpl>(s); return d ? d->sample_count : 1; };
    static auto mtl_rpl_bufs        = +[](void *s, const char *sel) -> void * {
        (void)s; (void)sel;
        static void *g_pb_arr = nullptr;
        if (!g_pb_arr) {
            g_pb_arr = oh2_objc_alloc_instance(oh2_objc_ensure_class("MTLPipelineBufferDescriptorArray", "NSObject"), 64);
            oh2_objc_mark_immortal(g_pb_arr);
        }
        return g_pb_arr;
    };
    static auto mtl_rpl_tess_flt    = +[](void *s, const char *sel) -> float { (void)s; (void)sel; return 16.0f; };
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "label",                         (void *)mtl_pip_label);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setLabel:",                     (void *)mtl_pip_noop_ptr);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "depthAttachmentPixelFormat",    (void *)mtl_rpl_get_depth);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "stencilAttachmentPixelFormat",  (void *)mtl_rpl_get_stencil);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "sampleCount",                   (void *)mtl_rpl_get_samp);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "rasterSampleCount",             (void *)mtl_rpl_get_samp);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setRasterSampleCount:",         (void *)&mtl_rpl_set_samples);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "isAlphaToCoverageEnabled",      (void *)&mtl_rpl_get_a2c);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setAlphaToCoverageEnabled:",    (void *)&mtl_rpl_set_a2c);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "isAlphaToOneEnabled",           (void *)mtl_pip_zero_u8);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setAlphaToOneEnabled:",         (void *)mtl_pip_noop_u8);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "isRasterizationEnabled",        (void *)mtl_pip_one_u8);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setRasterizationEnabled:",      (void *)mtl_pip_noop_u8);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "vertexBuffers",                 (void *)mtl_rpl_bufs);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "fragmentBuffers",               (void *)mtl_rpl_bufs);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "tessellationFactorStepFunction", (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setTessellationFactorStepFunction:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "tessellationOutputWindingOrder", (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setTessellationOutputWindingOrder:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "tessellationPartitionMode",     (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setTessellationPartitionMode:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "maxTessellationFactor",         (void *)mtl_rpl_tess_flt);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setMaxTessellationFactor:",     (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "isTessellationFactorScaleEnabled", (void *)mtl_pip_zero_u8);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setTessellationFactorScaleEnabled:", (void *)mtl_pip_noop_u8);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "tessellationFactorFormat",      (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setTessellationFactorFormat:",  (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "tessellationControlPointIndexType", (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "setTessellationControlPointIndexType:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineDescriptor", "reset",                         (void *)&mtl_rpl_reset);

    static auto mtl_pca_get_blend = +[](void *s, const char *sel) -> uint8_t  { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->blending_enabled : 0; };
    static auto mtl_pca_get_srgb  = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->src_rgb : 1; };
    static auto mtl_pca_get_drgb  = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->dst_rgb : 0; };
    static auto mtl_pca_get_rgbop = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->rgb_op : 0; };
    static auto mtl_pca_get_sa    = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->src_alpha : 1; };
    static auto mtl_pca_get_da    = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->dst_alpha : 0; };
    static auto mtl_pca_get_aop   = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->alpha_op : 0; };
    static auto mtl_pca_get_wmask = +[](void *s, const char *sel) -> uint64_t { (void)sel; auto *c = unwrap_mtl<MtlPipelineColorAttachDesc>(s); return c ? c->write_mask : 0xF; };

    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "init",                         (void *)&mtl_pca_init);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "copy",                         (void *)&mtl_pca_copy);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "copyWithZone:",                (void *)&mtl_pca_copyWithZone);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "pixelFormat",                  (void *)&mtl_pca_get_fmt);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setPixelFormat:",              (void *)&mtl_pca_set_fmt);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "isBlendingEnabled",            (void *)mtl_pca_get_blend);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "blendingEnabled",              (void *)mtl_pca_get_blend);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setBlendingEnabled:",          (void *)&mtl_pca_set_blend);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "sourceRGBBlendFactor",         (void *)mtl_pca_get_srgb);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setSourceRGBBlendFactor:",     (void *)&mtl_pca_set_srgb);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "destinationRGBBlendFactor",    (void *)mtl_pca_get_drgb);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setDestinationRGBBlendFactor:", (void *)&mtl_pca_set_drgb);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "rgbBlendOperation",            (void *)mtl_pca_get_rgbop);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setRgbBlendOperation:",        (void *)&mtl_pca_set_rgbop);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "sourceAlphaBlendFactor",       (void *)mtl_pca_get_sa);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setSourceAlphaBlendFactor:",   (void *)&mtl_pca_set_sa);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "destinationAlphaBlendFactor",  (void *)mtl_pca_get_da);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setDestinationAlphaBlendFactor:", (void *)&mtl_pca_set_da);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "alphaBlendOperation",          (void *)mtl_pca_get_aop);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setAlphaBlendOperation:",      (void *)&mtl_pca_set_aop);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "writeMask",                    (void *)mtl_pca_get_wmask);
    oh2_objc_register_instance_method("MTLRenderPipelineColorAttachmentDescriptor", "setWriteMask:",                (void *)&mtl_pca_set_wmask);

    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "init",                (void *)&mtl_cpd_init);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "copy",                (void *)&mtl_cpd_copy);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "copyWithZone:",       (void *)&mtl_cpd_copyWithZone);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "label",               (void *)mtl_pip_label);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "setLabel:",           (void *)mtl_pip_noop_ptr);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "computeFunction",     (void *)&mtl_cpd_get_fn);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "setComputeFunction:", (void *)&mtl_cpd_set_fn);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "threadGroupSizeIsMultipleOfThreadExecutionWidth", (void *)mtl_pip_zero_u8);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "setThreadGroupSizeIsMultipleOfThreadExecutionWidth:", (void *)mtl_pip_noop_u8);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "buffers",             (void *)mtl_rpl_bufs);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "stageInputDescriptor",(void *)mtl_pip_zero_ptr);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "setStageInputDescriptor:", (void *)mtl_pip_noop_ptr);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "setMaxTotalThreadsPerThreadgroup:", (void *)mtl_pip_noop_u64);
    oh2_objc_register_instance_method("MTLComputePipelineDescriptor", "reset",               (void *)&mtl_cpd_reset);

    oh2_objc_register_instance_method("MTLDevice", "newRenderPipelineStateWithDescriptor:error:",                   (void *)&mtl_device_newRenderPipelineStateErr);
    oh2_objc_register_instance_method("MTLDevice", "newRenderPipelineStateWithDescriptor:options:reflection:error:", (void *)&mtl_device_newRenderPipelineState);
    oh2_objc_register_instance_method("MTLDevice", "newComputePipelineStateWithFunction:error:",                  (void *)&mtl_device_newComputePipelineFnErr);
    oh2_objc_register_instance_method("MTLDevice", "newComputePipelineStateWithFunction:options:reflection:error:", (void *)&mtl_device_newComputePipelineWithFn);
    oh2_objc_register_instance_method("MTLDevice", "newComputePipelineStateWithDescriptor:options:reflection:error:", (void *)&mtl_device_newComputePipelineDesc);

    oh2_objc_register_instance_method("MTLRenderPipelineState",  "device",                        (void *)mtl_pip_device);
    oh2_objc_register_instance_method("MTLRenderPipelineState",  "label",                         (void *)mtl_pip_label);
    oh2_objc_register_instance_method("MTLRenderPipelineState",  "maxTotalThreadsPerThreadgroup", (void *)&mtl_cpso_maxThreads);
    oh2_objc_register_instance_method("MTLRenderPipelineState",  "imageblockMemoryLengthForDimensions:", (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLRenderPipelineState",  "threadgroupSizeMatchesTileSize", (void *)mtl_pip_zero_u8);
    oh2_objc_register_instance_method("MTLRenderPipelineState",  "imageblockSampleLength",        (void *)mtl_pip_zero_u64);
    oh2_objc_register_instance_method("MTLComputePipelineState", "device",                        (void *)mtl_pip_device);
    oh2_objc_register_instance_method("MTLComputePipelineState", "label",                         (void *)mtl_pip_label);
    oh2_objc_register_instance_method("MTLComputePipelineState", "maxTotalThreadsPerThreadgroup", (void *)&mtl_cpso_maxThreads);
    oh2_objc_register_instance_method("MTLComputePipelineState", "threadExecutionWidth",          (void *)&mtl_cpso_execWidth);
    oh2_objc_register_instance_method("MTLComputePipelineState", "staticThreadgroupMemoryLength", (void *)mtl_pip_zero_u64);
}

} // namespace oh2::mtlvk
