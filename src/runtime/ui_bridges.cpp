/*
 * ui_bridges.cpp — M2 UIKit, QuartzCore (CADisplayLink / CAMetalLayer),
 * and UIApplicationMain bridge with live UE4 MainAppThread -> oh2-ios.pak wait loop.
 */

#include "oh2_runtime.h"
#include "../loader/macho_loader.h"
#include "../libc_wrapper/libc_wrapper.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

extern "C" {
void oh2_objc_msgSend(void);
void oh2_dispatch_drain_main_queue(void);
}

struct CGRect64 {
    double x;
    double y;
    double width;
    double height;
};

struct DisplayLinkState {
    std::atomic<int> running{0};
    std::atomic<int> paused{0};
    std::atomic<int> preferred_fps{60};
    void            *target{nullptr};
    const char      *selector{nullptr};
    void            *self_obj{nullptr};
    double           timestamp{0.0};
    double           target_timestamp{0.0};
    pthread_t        thread{0};
};

namespace {

static std::mutex g_ui_mutex;
static void *g_ui_application = nullptr;
static void *g_ui_app_delegate = nullptr;
static void *g_ui_window      = nullptr;
static void *g_ui_screen      = nullptr;
static void *g_ui_device      = nullptr;
static void *g_root_vc        = nullptr;
static void *g_active_ios_view = nullptr;

static std::atomic<int>      g_live_loop_mode{0};
static std::atomic<int>      g_live_loop_running{0};
static std::atomic<uint32_t> g_screen_w_px{2340};
static std::atomic<uint32_t> g_screen_h_px{1080};
static double                g_screen_scale = 2.0;

static std::atomic<int>             g_vsync_driven{0};
static std::mutex                   g_dl_mutex;
static std::vector<DisplayLinkState *> g_display_links;

struct UIObjAssoc {
    CGRect64          frame{0.0, 0.0, 0.0, 0.0};
    void             *root_vc{nullptr};
    void             *vc_view{nullptr};
    void             *layer{nullptr};
    DisplayLinkState *dl_state{nullptr};
};

static std::unordered_map<void *, UIObjAssoc> g_ui_assoc;

static void *ensure_view_layer(void *self) {
    if (!self) return nullptr;
    {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        auto it = g_ui_assoc.find(self);
        if (it != g_ui_assoc.end() && it->second.layer) {
            return it->second.layer;
        }
    }
    void *self_cls = *(void **)self;
    void *layer_cls = nullptr;
    if (self_cls) {
        typedef void *(*layer_cls_fn_t)(void *, const char *);
        layer_cls = ((layer_cls_fn_t)oh2_objc_msgSend)(self_cls, "layerClass");
    }
    if (!layer_cls) {
        layer_cls = oh2_objc_get_class("CAMetalLayer");
    }
    typedef void *(*obj0_fn_t)(void *, const char *);
    void *layer = ((obj0_fn_t)oh2_objc_msgSend)(layer_cls, "alloc");
    layer = ((obj0_fn_t)oh2_objc_msgSend)(layer, "init");
    if (!layer) {
        layer = oh2_objc_alloc_instance(oh2_objc_get_class("CAMetalLayer"), 128);
    }
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    UIObjAssoc &a = g_ui_assoc[self];
    if (!a.layer) a.layer = layer;
    return a.layer;
}

/* UIApplication */
static void *m2_uiapp_sharedApplication(void *cls, const char *sel) {
    (void)cls; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    if (!g_ui_application) {
        g_ui_application = oh2_objc_alloc_instance(oh2_objc_get_class("UIApplication"), 64);
    }
    return g_ui_application;
}

static void *m2_uiapp_delegate(void *self, const char *sel) {
    (void)self; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    return g_ui_app_delegate;
}

static void m2_uiapp_setDelegate(void *self, const char *sel, void *del) {
    (void)self; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    g_ui_app_delegate = del;
}

static void *m2_uiapp_keyWindow(void *self, const char *sel) {
    (void)self; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    return g_ui_window;
}

static uint8_t m2_uiapp_openURL(void *self, const char *sel, void *url) {
    (void)self; (void)sel; (void)url;
    return 0;
}

static void m2_uiview_setTag(void *self, const char *sel, int64_t tag) {
    (void)self; (void)sel; (void)tag;
}

static void *m2_uiimage_CGImage(void *self, const char *sel) {
    (void)self; (void)sel;
    return nullptr;
}

static int64_t m2_uiapp_applicationState(void *self, const char *sel) {
    (void)self; (void)sel;
    return 0; /* UIApplicationStateActive */
}

static int64_t m2_uiapp_statusBarOrientation(void *self, const char *sel) {
    (void)self; (void)sel;
    return 3; /* UIInterfaceOrientationLandscapeRight */
}

static void m2_uiapp_noop_1(void *self, const char *sel, uint64_t arg) {
    (void)self; (void)sel; (void)arg;
}

/* UIWindow & UIView */
static void maybe_capture_ios_view(void *obj) {
    if (!obj) return;
    const char *cname = oh2_objc_object_class_name(obj);
    if (cname && (strcmp(cname, "FIOSView") == 0 || strstr(cname, "IOSView") != nullptr)) {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        g_active_ios_view = obj;
    }
}

static void *m2_uiview_initWithFrame(void *self, const char *sel, CGRect64 frame) {
    (void)sel;
    if (!self) return nullptr;
    {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        g_ui_assoc[self].frame = frame;
        if (*(void **)self == oh2_objc_get_class("UIWindow")) {
            g_ui_window = self;
        }
    }
    maybe_capture_ios_view(self);
    ensure_view_layer(self);
    return self;
}

static CGRect64 m2_uiview_bounds(void *self, const char *sel) {
    (void)sel;
    if (self) {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        auto it = g_ui_assoc.find(self);
        if (it != g_ui_assoc.end() && it->second.frame.width > 0.0 && it->second.frame.height > 0.0) {
            return it->second.frame;
        }
    }
    double sc = (g_screen_scale > 0.0) ? g_screen_scale : 2.0;
    return {0.0, 0.0, (double)g_screen_w_px.load() / sc, (double)g_screen_h_px.load() / sc};
}

static void m2_uiview_setFrame(void *self, const char *sel, CGRect64 frame) {
    (void)sel;
    if (self) {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        g_ui_assoc[self].frame = frame;
    }
}

static void m2_uiview_addSubview(void *self, const char *sel, void *subview) {
    (void)self; (void)sel;
    maybe_capture_ios_view(subview);
}

static void *m2_uiview_window(void *self, const char *sel) {
    (void)self; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    return g_ui_window;
}

static void *m2_uiview_viewWithTag(void *self, const char *sel, int64_t tag) {
    (void)self; (void)sel; (void)tag;
    return nullptr;
}

static void m2_uiview_noop_u64(void *self, const char *sel, uint64_t v) {
    (void)self; (void)sel; (void)v;
}

static void m2_uiview_noop_u8(void *self, const char *sel, uint8_t v) {
    (void)self; (void)sel; (void)v;
}

static double m2_uiview_contentScaleFactor(void *self, const char *sel) {
    (void)self; (void)sel;
    return g_screen_scale > 0.0 ? g_screen_scale : 2.0;
}

static void m2_uiview_setContentScaleFactor(void *self, const char *sel, double scale) {
    (void)self; (void)sel; (void)scale;
}

static void m2_uiview_setMultipleTouchEnabled(void *self, const char *sel, uint8_t enabled) {
    (void)self; (void)sel; (void)enabled;
}

static void m2_uiview_setNeedsDisplay(void *self, const char *sel) {
    (void)self; (void)sel;
}

static void *m2_uiview_layer(void *self, const char *sel) {
    (void)sel;
    return ensure_view_layer(self);
}

static void *m2_uiview_layerClass(void *cls, const char *sel) {
    (void)cls; (void)sel;
    return oh2_objc_get_class("CAMetalLayer");
}

static void m2_uiwindow_makeKeyAndVisible(void *self, const char *sel) {
    (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    g_ui_window = self;
}

static void *m2_uiwindow_rootViewController(void *self, const char *sel) {
    (void)self; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    return g_root_vc;
}

static void m2_uiwindow_setRootViewController(void *self, const char *sel, void *vc) {
    (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    g_root_vc = vc;
    if (self) g_ui_assoc[self].root_vc = vc;
}

static void m2_uiwindow_setScreen(void *self, const char *sel, void *screen) {
    (void)self; (void)sel; (void)screen;
}

static void *m2_uiscreen_mainScreen(void *cls, const char *sel);

static void *m2_uiwindow_screen(void *self, const char *sel) {
    (void)self; (void)sel;
    return m2_uiscreen_mainScreen(oh2_objc_get_class("UIScreen"), "mainScreen");
}

/* UIViewController */
static void *m2_uivc_view(void *self, const char *sel) {
    (void)sel;
    if (!self) return nullptr;
    {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        auto it = g_ui_assoc.find(self);
        if (it != g_ui_assoc.end() && it->second.vc_view) {
            return it->second.vc_view;
        }
    }
    void *v = oh2_objc_alloc_instance(oh2_objc_get_class("UIView"), 64);
    {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        UIObjAssoc &a = g_ui_assoc[self];
        if (!a.vc_view) {
            a.vc_view = v;
            g_ui_assoc[v].frame = {0.0, 0.0, 2340.0, 1080.0};
        }
        v = a.vc_view;
    }
    return v;
}

static void m2_uivc_setView(void *self, const char *sel, void *v) {
    (void)sel;
    if (self) {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        g_ui_assoc[self].vc_view = v;
    }
    maybe_capture_ios_view(v);
}

static void m2_uivc_viewDidLoad(void *self, const char *sel) {
    (void)self; (void)sel;
}

static void m2_uivc_viewWillAppear(void *self, const char *sel, uint8_t animated) {
    (void)self; (void)sel; (void)animated;
}

struct MinimalBlockHead {
    void *isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke)(void *);
};

static void m2_uivc_presentViewController(void *self, const char *sel, void *vc,
                                          uint8_t animated, void *completion) {
    (void)self; (void)sel; (void)vc; (void)animated;
    oh2_log_info("[M2 UI] presentViewController: %p", vc);
    if (completion) {
        MinimalBlockHead *b = (MinimalBlockHead *)completion;
        if (b->invoke) b->invoke(completion);
    }
}

static void *m2_uialert_withTitle(void *cls, const char *sel, void *title, void *msg, int64_t style) {
    (void)cls; (void)sel;
    typedef const char *(*utf8_fn_t)(void *, const char *);
    const char *t_str = title ? ((utf8_fn_t)oh2_objc_msgSend)(title, "UTF8String") : "(null)";
    const char *m_str = msg   ? ((utf8_fn_t)oh2_objc_msgSend)(msg,   "UTF8String") : "(null)";
    oh2_log_error("[M2 UIAlertController] style=%lld title='%s' message='%s'",
                  (long long)style, t_str ? t_str : "(null)", m_str ? m_str : "(null)");
    return oh2_objc_alloc_instance(oh2_objc_get_class("UIAlertController"), 64);
}

static void m2_uialert_addAction(void *self, const char *sel, void *action) {
    (void)self; (void)sel; (void)action;
}

static void *m2_uialertaction_withTitle(void *cls, const char *sel, void *title, int64_t style, void *handler) {
    (void)cls; (void)sel; (void)title; (void)style; (void)handler;
    return oh2_objc_alloc_instance(oh2_objc_get_class("UIAlertAction"), 64);
}

/* UIImage / UIImageView */
static void *m2_uiimage_initWithContentsOfFile(void *self, const char *sel, void *path) {
    (void)sel; (void)path;
    return self;
}

static void *m2_uiimage_imageWithCGImage(void *cls, const char *sel, void *cgimg, double scale, int64_t orient) {
    (void)cls; (void)sel; (void)cgimg; (void)scale; (void)orient;
    return oh2_objc_alloc_instance(oh2_objc_get_class("UIImage"), 64);
}

static void *m2_uiimageview_initWithImage(void *self, const char *sel, void *img) {
    (void)sel; (void)img;
    return self;
}

/* UIScreen */
static void *m2_uiscreen_mainScreen(void *cls, const char *sel) {
    (void)cls; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    if (!g_ui_screen) {
        g_ui_screen = oh2_objc_alloc_instance(oh2_objc_get_class("UIScreen"), 64);
    }
    double sc = (g_screen_scale > 0.0) ? g_screen_scale : 2.0;
    g_ui_assoc[g_ui_screen].frame = {
        0.0, 0.0, (double)g_screen_w_px.load() / sc, (double)g_screen_h_px.load() / sc
    };
    return g_ui_screen;
}

static CGRect64 m2_uiscreen_bounds(void *self, const char *sel) {
    (void)self; (void)sel;
    double sc = (g_screen_scale > 0.0) ? g_screen_scale : 2.0;
    return {0.0, 0.0, (double)g_screen_w_px.load() / sc, (double)g_screen_h_px.load() / sc};
}

static CGRect64 m2_uiscreen_nativeBounds(void *self, const char *sel) {
    (void)self; (void)sel;
    return {0.0, 0.0, (double)g_screen_w_px.load(), (double)g_screen_h_px.load()};
}

static double m2_uiscreen_scale(void *self, const char *sel) {
    (void)self; (void)sel;
    return g_screen_scale > 0.0 ? g_screen_scale : 2.0;
}

static double m2_uiscreen_brightness(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1.0;
}

static void *g_trait_collection = nullptr;
static void *m2_uiscreen_traitCollection(void *self, const char *sel) {
    (void)self; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    if (!g_trait_collection) {
        g_trait_collection = oh2_objc_alloc_instance(oh2_objc_get_class("UITraitCollection"), 64);
    }
    return g_trait_collection;
}
static int64_t m2_uitrait_idiom(void *self, const char *sel) { (void)self; (void)sel; return 0; }
static int64_t m2_uitrait_hSize(void *self, const char *sel) { (void)self; (void)sel; return 2; }
static int64_t m2_uitrait_vSize(void *self, const char *sel) { (void)self; (void)sel; return 1; }
static double m2_uitrait_scale(void *self, const char *sel) { (void)self; (void)sel; return g_screen_scale > 0.0 ? g_screen_scale : 2.0; }

/* UIDevice */
static void *m2_uidevice_currentDevice(void *cls, const char *sel) {
    (void)cls; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    if (!g_ui_device) {
        g_ui_device = oh2_objc_alloc_instance(oh2_objc_get_class("UIDevice"), 64);
    }
    return g_ui_device;
}

static void *m2_uidevice_model(void *self, const char *sel) {
    (void)self; (void)sel;
    return oh2_objc_create_string("iPhone");
}

static void *m2_uidevice_systemVersion(void *self, const char *sel) {
    (void)self; (void)sel;
    return oh2_objc_create_string("16.0");
}

static int64_t m2_uidevice_userInterfaceIdiom(void *self, const char *sel) {
    (void)self; (void)sel;
    return 0; /* UIUserInterfaceIdiomPhone */
}

static void m2_uidevice_noop(void *self, const char *sel) {
    (void)self; (void)sel;
}

static void m2_uidevice_setBool(void *self, const char *sel, uint8_t val) {
    (void)self; (void)sel; (void)val;
}

/* CADisplayLink */
static void *cadl_worker(void *arg) {
    DisplayLinkState *st = (DisplayLinkState *)arg;
    oh2_darwin_pthread_setname_np("CADisplayLink");

    while (st->running.load()) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
        int fps = st->preferred_fps.load();
        if (fps <= 0) fps = 60;
        double period = 1.0 / (double)fps;
        st->timestamp = now;
        st->target_timestamp = now + period;

        if (!st->paused.load() && st->target && st->selector) {
            typedef void (*msg_fn_t)(void *, const char *, void *);
            ((msg_fn_t)oh2_objc_msgSend)(st->target, st->selector, st->self_obj);
        }

        struct timespec req;
        long ns = (long)(period * 1e9);
        req.tv_sec  = ns / 1000000000L;
        req.tv_nsec = ns % 1000000000L;
        nanosleep(&req, nullptr);
    }
    return nullptr;
}

static DisplayLinkState *get_dl_state(void *self) {
    if (!self) return nullptr;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    auto it = g_ui_assoc.find(self);
    return (it != g_ui_assoc.end()) ? it->second.dl_state : nullptr;
}

static void *m2_cadl_displayLinkWithTarget(void *cls, const char *sel, void *target, const char *selector) {
    (void)cls; (void)sel;
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("CADisplayLink"), 64);
    if (!obj) return nullptr;
    DisplayLinkState *st = new DisplayLinkState();
    st->running.store(1);
    st->paused.store(0);
    st->preferred_fps.store(60);
    st->target   = target;
    st->selector = selector ? oh2_objc_intern_selector(selector) : nullptr;
    st->self_obj = obj;
    {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        g_ui_assoc[obj].dl_state = st;
    }
    return obj;
}

static void m2_cadl_addToRunLoop(void *self, const char *sel, void *runloop, void *mode) {
    (void)sel; (void)runloop; (void)mode;
    DisplayLinkState *st = get_dl_state(self);
    if (!st) return;
    {
        std::lock_guard<std::mutex> lk(g_dl_mutex);
        if (std::find(g_display_links.begin(), g_display_links.end(), st) == g_display_links.end()) {
            g_display_links.push_back(st);
        }
    }
    st->running.store(1);
    if (g_vsync_driven.load()) {
        return; /* Driven externally by AChoreographer on VSYNC */
    }
    if (st->thread) return;
    oh2_darwin_pthread_create(&st->thread, nullptr, cadl_worker, st);
}

static void m2_cadl_setPreferredFPS(void *self, const char *sel, int64_t fps) {
    (void)sel;
    DisplayLinkState *st = get_dl_state(self);
    if (st) st->preferred_fps.store(fps > 0 ? (int)fps : 60);
}

static void m2_cadl_setPaused(void *self, const char *sel, uint8_t paused) {
    (void)sel;
    DisplayLinkState *st = get_dl_state(self);
    if (st) st->paused.store(paused ? 1 : 0);
}

static void m2_cadl_invalidate(void *self, const char *sel) {
    (void)sel;
    DisplayLinkState *st = get_dl_state(self);
    if (!st) return;
    st->running.store(0);
    {
        std::lock_guard<std::mutex> lk(g_dl_mutex);
        g_display_links.erase(std::remove(g_display_links.begin(), g_display_links.end(), st), g_display_links.end());
    }
    if (st->thread) {
        pthread_join(st->thread, nullptr);
        st->thread = 0;
    }
}

static double m2_cadl_timestamp(void *self, const char *sel) {
    (void)sel;
    DisplayLinkState *st = get_dl_state(self);
    return st ? st->timestamp : 0.0;
}

static double m2_cadl_targetTimestamp(void *self, const char *sel) {
    (void)sel;
    DisplayLinkState *st = get_dl_state(self);
    return st ? st->target_timestamp : 0.0;
}

static int64_t m2_cadl_preferredFPS(void *self, const char *sel) {
    (void)sel;
    DisplayLinkState *st = get_dl_state(self);
    return st ? st->preferred_fps.load() : 60;
}

/* UIView / UIResponder / UIFont / UIPasteboard / Haptics M5 helpers */
struct UIEdgeInsets64 { double top, left, bottom, right; };
static uint8_t m2_ui_yes(void *s, const char *sel) { (void)s; (void)sel; return 1; }
static uint8_t m2_ui_no(void *s, const char *sel) { (void)s; (void)sel; return 0; }
static uint8_t m2_ui_endEditing(void *s, const char *sel, uint8_t force) { (void)s; (void)sel; (void)force; return 1; }
static void m2_ui_noop0(void *s, const char *sel) { (void)s; (void)sel; }
static void m2_ui_noop1(void *s, const char *sel, void *a) { (void)s; (void)sel; (void)a; }
static void m2_ui_dismissVC(void *s, const char *sel, uint8_t anim, void *blk) {
    (void)s; (void)sel; (void)anim;
    if (blk) {
        struct Blk0 { void *isa; int32_t f, r; void (*inv)(void *); };
        auto *b = (Blk0 *)blk;
        if (b->inv) b->inv(blk);
    }
}
static void *m2_ui_ret_nil(void *s, const char *sel) { (void)s; (void)sel; return nullptr; }
static UIEdgeInsets64 m2_uiview_safeAreaInsets(void *s, const char *sel) { (void)s; (void)sel; return {0,0,0,0}; }
static double m2_uifont_sysSize(void *c, const char *sel) { (void)c; (void)sel; return 17.0; }
static void *m2_uifont_sysFontOfSize(void *c, const char *sel, double sz) {
    (void)sel; (void)sz;
    return oh2_objc_alloc_instance(c, 64);
}
static void *m2_uifont_fontName(void *s, const char *sel) { (void)s; (void)sel; return oh2_objc_create_string("Helvetica"); }
static void *g_ui_pasteboard = nullptr;
static void *m2_uipb_general(void *c, const char *sel) {
    (void)sel;
    if (!g_ui_pasteboard) g_ui_pasteboard = oh2_objc_alloc_instance(c, 64);
    return g_ui_pasteboard;
}
static void *m2_ui_initWithStyle(void *s, const char *sel, int64_t style) { (void)sel; (void)style; return s; }

static double m2_cadl_duration(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1.0 / 60.0;
}

/* CAMetalLayer stubs so UIView/FIOSView inspection never hits missing selectors */
static void *m2_cametal_device(void *self, const char *sel) {
    (void)self; (void)sel;
    return nullptr;
}
static void m2_cametal_setProp(void *self, const char *sel, uint64_t v) {
    (void)self; (void)sel; (void)v;
}
static void m2_cametal_setSize(void *self, const char *sel, double w, double h) {
    (void)self; (void)sel; (void)w; (void)h;
}

/* M2/M5 UIApplicationMain orchestrating real MainAppThread */
static int m2_UIApplicationMain(int argc, char **argv, void *principalClassName, void *delegateClassName) {
    (void)argc; (void)argv; (void)principalClassName;

    oh2_set_ios_main_thread();

    char scratch[128];
    const char *del_name = darwin_cfstring_utf8(delegateClassName, scratch, sizeof(scratch));
    if (!del_name || !del_name[0] || strcmp(del_name, "(null)") == 0 || strcmp(del_name, "<Object>") == 0) {
        del_name = "IOSAppDelegate";
    }
    oh2_log_info("[M2] UIApplicationMain launching delegate '%s' (live_loop=%d)",
                 del_name, g_live_loop_mode.load());

    void *app = m2_uiapp_sharedApplication(oh2_objc_get_class("UIApplication"), "sharedApplication");
    void *del_cls = oh2_objc_get_class(del_name);
    if (!del_cls) del_cls = oh2_objc_get_class("NSObject");

    typedef void *(*alloc_fn_t)(void *, const char *);
    typedef void *(*init_fn_t)(void *, const char *);
    void *app_delegate = ((alloc_fn_t)oh2_objc_msgSend)(del_cls, "alloc");
    app_delegate = ((init_fn_t)oh2_objc_msgSend)(app_delegate, "init");
    m2_uiapp_setDelegate(app, "setDelegate:", app_delegate);

    oh2_pak_telemetry_reset();
    oh2_runtime_reset_metal_boundary();

    typedef uint8_t (*did_finish_t)(void *, const char *, void *, void *);
    const char *did_finish_sel = oh2_objc_intern_selector("application:didFinishLaunchingWithOptions:");
    did_finish_t did_finish_fn = (did_finish_t)oh2_objc_lookup_imp(app_delegate, did_finish_sel);
    uint8_t launched = did_finish_fn(app_delegate, did_finish_sel, app, nullptr);
    oh2_log_info("[M2] -[IOSAppDelegate application:didFinishLaunchingWithOptions:] -> %u", (unsigned)launched);

    /* Real iOS UIKit delivers applicationDidBecomeActive: right after launch finishes */
    typedef void (*became_active_t)(void *, const char *, void *);
    const char *active_sel = oh2_objc_intern_selector("applicationDidBecomeActive:");
    void *active_imp = oh2_objc_lookup_imp(app_delegate, active_sel);
    if (active_imp) {
        ((became_active_t)oh2_objc_msgSend)(app_delegate, active_sel, app);
        oh2_log_info("[M2] Delivered applicationDidBecomeActive: to delegate");
    }

    if (g_live_loop_mode.load()) {
        g_live_loop_running.store(1);
        oh2_log_info("[M5] Entering live UIApplicationMain runloop pump");
        while (g_live_loop_running.load()) {
            oh2_dispatch_drain_main_queue();
            oh2_runtime_drain_timers();
            usleep(2000);
        }
        oh2_log_info("[M5] Exited live UIApplicationMain runloop pump");
        return 0;
    }

    int pak_ok = oh2_runtime_wait_for_pak_or_boundary(30000);
    Oh2PakIoTelemetry io;
    oh2_pak_telemetry_get(&io);
    oh2_log_info("[M2] MainAppThread wait complete (ok=%d): opened=%d fstat_size=%llu footer=%d index_bytes=%llu index_complete=%d asset_reads=%llu metal_boundary=%d",
                 pak_ok, io.opened_by_ue4,
                 (unsigned long long)io.fstat_size, io.footer_read,
                 (unsigned long long)io.index_bytes_read, io.index_read_complete,
                 (unsigned long long)io.asset_reads_after_index,
                 oh2_runtime_did_hit_metal_boundary());

    return pak_ok ? 0 : 1;
}

} // namespace

extern "C" {

void oh2_runtime_set_live_loop_mode(int enable) {
    g_live_loop_mode.store(enable ? 1 : 0);
}

void oh2_runtime_stop_live_loop(void) {
    g_live_loop_running.store(0);
}

int oh2_runtime_is_live_loop_running(void) {
    return (g_live_loop_mode.load() && g_live_loop_running.load()) ? 1 : 0;
}

void oh2_ui_set_screen_dimensions(uint32_t width_px, uint32_t height_px, double scale) {
    if (width_px > 0) g_screen_w_px.store(width_px);
    if (height_px > 0) g_screen_h_px.store(height_px);
    if (scale > 0.0) g_screen_scale = scale;
}

void oh2_ui_get_screen_dimensions(uint32_t *out_w_px, uint32_t *out_h_px, double *out_scale) {
    if (out_w_px) *out_w_px = g_screen_w_px.load();
    if (out_h_px) *out_h_px = g_screen_h_px.load();
    if (out_scale) *out_scale = g_screen_scale;
}

void *oh2_ui_get_active_ios_view(void) {
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    if (g_active_ios_view) return g_active_ios_view;
    if (g_root_vc) return m2_uivc_view(g_root_vc, "view");
    return nullptr;
}

void oh2_ui_set_active_ios_view(void *view) {
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    g_active_ios_view = view;
}

void *oh2_ui_get_active_window(void) {
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    return g_ui_window;
}

void oh2_cadisplaylink_set_vsync_driven(int enable) {
    g_vsync_driven.store(enable ? 1 : 0);
}

void oh2_cadisplaylink_tick_all(double timestamp_sec, double target_timestamp_sec) {
    std::vector<DisplayLinkState *> links;
    {
        std::lock_guard<std::mutex> lk(g_dl_mutex);
        links = g_display_links;
    }
    typedef void (*msg_fn_t)(void *, const char *, void *);
    for (DisplayLinkState *st : links) {
        if (!st || !st->running.load() || st->paused.load() || !st->target || !st->selector) {
            continue;
        }
        st->timestamp = timestamp_sec;
        st->target_timestamp = (target_timestamp_sec > timestamp_sec)
                                   ? target_timestamp_sec
                                   : (timestamp_sec + (1.0 / (double)(st->preferred_fps.load() > 0 ? st->preferred_fps.load() : 60)));
        ((msg_fn_t)oh2_objc_msgSend)(st->target, st->selector, st->self_obj);
    }
}

void oh2_m2_register_ui_bridges(void) {
    /* UIApplication */
    oh2_objc_register_class_method("UIApplication",    "sharedApplication",           (void *)&m2_uiapp_sharedApplication);
    oh2_objc_register_instance_method("UIApplication", "delegate",                    (void *)&m2_uiapp_delegate);
    oh2_objc_register_instance_method("UIApplication", "setDelegate:",                (void *)&m2_uiapp_setDelegate);
    oh2_objc_register_instance_method("UIApplication", "keyWindow",                   (void *)&m2_uiapp_keyWindow);
    oh2_objc_register_instance_method("UIApplication", "openURL:",                    (void *)&m2_uiapp_openURL);
    oh2_objc_register_instance_method("UIApplication", "applicationState",            (void *)&m2_uiapp_applicationState);
    oh2_objc_register_instance_method("UIApplication", "statusBarOrientation",        (void *)&m2_uiapp_statusBarOrientation);
    oh2_objc_register_instance_method("UIApplication", "setApplicationIconBadgeNumber:", (void *)&m2_uiapp_noop_1);
    oh2_objc_register_instance_method("UIApplication", "setIdleTimerDisabled:",       (void *)&m2_uiapp_noop_1);
    oh2_objc_register_instance_method("UIApplication", "isIdleTimerDisabled",         (void *)&m2_ui_no);
    oh2_objc_register_instance_method("UIApplication", "canOpenURL:",                 (void *)&m2_ui_no);

    /* UIWindow */
    oh2_objc_register_instance_method("UIWindow", "initWithFrame:",          (void *)&m2_uiview_initWithFrame);
    oh2_objc_register_instance_method("UIWindow", "makeKeyAndVisible",       (void *)&m2_uiwindow_makeKeyAndVisible);
    oh2_objc_register_instance_method("UIWindow", "rootViewController",      (void *)&m2_uiwindow_rootViewController);
    oh2_objc_register_instance_method("UIWindow", "setRootViewController:",  (void *)&m2_uiwindow_setRootViewController);
    oh2_objc_register_instance_method("UIWindow", "screen",                  (void *)&m2_uiwindow_screen);
    oh2_objc_register_instance_method("UIWindow", "setScreen:",              (void *)&m2_uiwindow_setScreen);

    /* UIView */
    oh2_objc_register_class_method("UIView",       "layerClass",      (void *)&m2_uiview_layerClass);
    oh2_objc_register_instance_method("UIView",    "initWithFrame:",  (void *)&m2_uiview_initWithFrame);
    oh2_objc_register_instance_method("UIView",    "bounds",          (void *)&m2_uiview_bounds);
    oh2_objc_register_instance_method("UIView",    "frame",           (void *)&m2_uiview_bounds);
    oh2_objc_register_instance_method("UIView",    "setFrame:",       (void *)&m2_uiview_setFrame);
    oh2_objc_register_instance_method("UIView",    "addSubview:",     (void *)&m2_uiview_addSubview);
    oh2_objc_register_instance_method("UIView",    "window",          (void *)&m2_uiview_window);
    oh2_objc_register_instance_method("UIView",    "viewWithTag:",    (void *)&m2_uiview_viewWithTag);
    oh2_objc_register_instance_method("UIView",    "setAutoresizingMask:",         (void *)&m2_uiview_noop_u64);
    oh2_objc_register_instance_method("UIView",    "setBackgroundColor:",          (void *)&m2_uiview_noop_u64);
    oh2_objc_register_instance_method("UIView",    "setClearsContextBeforeDrawing:", (void *)&m2_uiview_noop_u8);
    oh2_objc_register_instance_method("UIView",    "setNeedsDisplay", (void *)&m2_uiview_setNeedsDisplay);
    oh2_objc_register_instance_method("UIView",    "setTag:",         (void *)&m2_uiview_setTag);
    oh2_objc_register_instance_method("UIView",    "contentScaleFactor",       (void *)&m2_uiview_contentScaleFactor);
    oh2_objc_register_instance_method("UIView",    "setContentScaleFactor:",   (void *)&m2_uiview_setContentScaleFactor);
    oh2_objc_register_instance_method("UIView",    "setMultipleTouchEnabled:", (void *)&m2_uiview_setMultipleTouchEnabled);
    oh2_objc_register_instance_method("UIView",    "layer",           (void *)&m2_uiview_layer);
    oh2_objc_register_instance_method("UIView",    "removeFromSuperview",      (void *)&m2_ui_noop0);
    oh2_objc_register_instance_method("UIView",    "becomeFirstResponder",     (void *)&m2_ui_yes);
    oh2_objc_register_instance_method("UIView",    "resignFirstResponder",     (void *)&m2_ui_yes);
    oh2_objc_register_instance_method("UIView",    "canBecomeFirstResponder",  (void *)&m2_ui_yes);
    oh2_objc_register_instance_method("UIView",    "nextResponder",            (void *)&m2_ui_ret_nil);
    oh2_objc_register_instance_method("UIView",    "setWindow:",               (void *)&m2_ui_noop1);
    oh2_objc_register_instance_method("UIView",    "safeAreaInsets",           (void *)&m2_uiview_safeAreaInsets);
    oh2_objc_register_instance_method("UIView",    "endEditing:",              (void *)&m2_ui_endEditing);
    oh2_objc_register_instance_method("UIView",    "accessibilityFrame",       (void *)&m2_uiview_bounds);
    oh2_objc_register_instance_method("UIResponder", "becomeFirstResponder",   (void *)&m2_ui_yes);
    oh2_objc_register_instance_method("UIResponder", "resignFirstResponder",   (void *)&m2_ui_yes);
    oh2_objc_register_instance_method("UIResponder", "nextResponder",          (void *)&m2_ui_ret_nil);

    /* UIViewController */
    oh2_objc_register_instance_method("UIViewController", "view",                                    (void *)&m2_uivc_view);
    oh2_objc_register_instance_method("UIViewController", "setView:",                                (void *)&m2_uivc_setView);
    oh2_objc_register_instance_method("UIViewController", "viewDidLoad",                             (void *)&m2_uivc_viewDidLoad);
    oh2_objc_register_instance_method("UIViewController", "viewWillAppear:",                         (void *)&m2_uivc_viewWillAppear);
    oh2_objc_register_instance_method("UIViewController", "setEdgesForExtendedLayout:",              (void *)&m2_uiview_noop_u64);
    oh2_objc_register_instance_method("UIViewController", "setModalPresentationStyle:",              (void *)&m2_uiview_noop_u64);
    oh2_objc_register_instance_method("UIViewController", "popoverPresentationController",           (void *)&m2_ui_ret_nil);
    oh2_objc_register_instance_method("UIViewController", "dismissViewControllerAnimated:completion:", (void *)&m2_ui_dismissVC);
    oh2_objc_register_instance_method("UIViewController", "presentViewController:animated:completion:", (void *)&m2_uivc_presentViewController);
    oh2_objc_register_instance_method("UIActivityViewController", "initWithActivityItems:applicationActivities:", (void *)&m2_uiview_noop_u64);
    oh2_objc_register_instance_method("UIActivityViewController", "setExcludedActivityTypes:", (void *)&m2_ui_noop1);

    /* UIAlertController / UIAlertAction */
    oh2_objc_register_class_method("UIAlertController",    "alertControllerWithTitle:message:preferredStyle:", (void *)&m2_uialert_withTitle);
    oh2_objc_register_instance_method("UIAlertController", "addAction:",                                        (void *)&m2_uialert_addAction);
    oh2_objc_register_class_method("UIAlertAction",        "actionWithTitle:style:handler:",                    (void *)&m2_uialertaction_withTitle);

    /* UIImage / UIImageView */
    oh2_objc_register_instance_method("UIImage",     "initWithContentsOfFile:",               (void *)&m2_uiimage_initWithContentsOfFile);
    oh2_objc_register_instance_method("UIImage",     "CGImage",                               (void *)&m2_uiimage_CGImage);
    oh2_objc_register_class_method("UIImage",        "imageWithCGImage:scale:orientation:",   (void *)&m2_uiimage_imageWithCGImage);
    oh2_objc_register_instance_method("UIImageView", "initWithImage:",                        (void *)&m2_uiimageview_initWithImage);

    /* UIScreen */
    oh2_objc_register_class_method("UIScreen",    "mainScreen",   (void *)&m2_uiscreen_mainScreen);
    oh2_objc_register_instance_method("UIScreen", "bounds",       (void *)&m2_uiscreen_bounds);
    oh2_objc_register_instance_method("UIScreen", "nativeBounds", (void *)&m2_uiscreen_nativeBounds);
    oh2_objc_register_instance_method("UIScreen", "scale",           (void *)&m2_uiscreen_scale);
    oh2_objc_register_instance_method("UIScreen", "nativeScale",     (void *)&m2_uiscreen_scale);
    oh2_objc_register_instance_method("UIScreen", "brightness",      (void *)&m2_uiscreen_brightness);
    oh2_objc_register_instance_method("UIScreen", "setBrightness:",  (void *)&m2_cametal_setSize);
    oh2_objc_register_instance_method("UIScreen", "traitCollection", (void *)&m2_uiscreen_traitCollection);

    /* UIFont, UIPasteboard, Haptic Feedback */
    oh2_objc_register_class_method("UIFont", "systemFontSize",       (void *)&m2_uifont_sysSize);
    oh2_objc_register_class_method("UIFont", "systemFontOfSize:",    (void *)&m2_uifont_sysFontOfSize);
    oh2_objc_register_instance_method("UIFont", "fontName",          (void *)&m2_uifont_fontName);
    oh2_objc_register_class_method("UIPasteboard", "generalPasteboard", (void *)&m2_uipb_general);
    oh2_objc_register_instance_method("UIImpactFeedbackGenerator", "initWithStyle:", (void *)&m2_ui_initWithStyle);
    oh2_objc_register_instance_method("UIImpactFeedbackGenerator", "prepare",        (void *)&m2_ui_noop0);
    oh2_objc_register_instance_method("UIImpactFeedbackGenerator", "impactOccurred", (void *)&m2_ui_noop0);
    oh2_objc_register_instance_method("UINotificationFeedbackGenerator", "prepare",              (void *)&m2_ui_noop0);
    oh2_objc_register_instance_method("UINotificationFeedbackGenerator", "notificationOccurred:",(void *)&m2_uiview_noop_u64);
    oh2_objc_register_instance_method("UISelectionFeedbackGenerator", "prepare",                 (void *)&m2_ui_noop0);
    oh2_objc_register_instance_method("UISelectionFeedbackGenerator", "selectionChanged",        (void *)&m2_ui_noop0);

    oh2_objc_ensure_class("UITraitCollection", "NSObject");
    oh2_objc_register_instance_method("UITraitCollection", "userInterfaceIdiom",  (void *)&m2_uitrait_idiom);
    oh2_objc_register_instance_method("UITraitCollection", "horizontalSizeClass", (void *)&m2_uitrait_hSize);
    oh2_objc_register_instance_method("UITraitCollection", "verticalSizeClass",   (void *)&m2_uitrait_vSize);
    oh2_objc_register_instance_method("UITraitCollection", "displayScale",        (void *)&m2_uitrait_scale);
    oh2_objc_register_instance_method("UITraitCollection", "displayGamut",        (void *)&m2_uitrait_idiom);

    /* UIDevice */
    oh2_objc_register_class_method("UIDevice",    "currentDevice",                          (void *)&m2_uidevice_currentDevice);
    oh2_objc_register_instance_method("UIDevice", "model",                                  (void *)&m2_uidevice_model);
    oh2_objc_register_instance_method("UIDevice", "systemVersion",                          (void *)&m2_uidevice_systemVersion);
    oh2_objc_register_instance_method("UIDevice", "userInterfaceIdiom",                     (void *)&m2_uidevice_userInterfaceIdiom);
    oh2_objc_register_instance_method("UIDevice", "beginGeneratingDeviceOrientationNotifications", (void *)&m2_uidevice_noop);
    oh2_objc_register_instance_method("UIDevice", "setBatteryMonitoringEnabled:",           (void *)&m2_uidevice_setBool);
    oh2_objc_register_instance_method("UIDevice", "identifierForVendor",                    (void *)&m2_ui_ret_nil);

    /* CADisplayLink */
    oh2_objc_register_class_method("CADisplayLink",    "displayLinkWithTarget:selector:", (void *)&m2_cadl_displayLinkWithTarget);
    oh2_objc_register_instance_method("CADisplayLink", "addToRunLoop:forMode:",          (void *)&m2_cadl_addToRunLoop);
    oh2_objc_register_instance_method("CADisplayLink", "preferredFramesPerSecond",       (void *)&m2_cadl_preferredFPS);
    oh2_objc_register_instance_method("CADisplayLink", "setPreferredFramesPerSecond:",    (void *)&m2_cadl_setPreferredFPS);
    oh2_objc_register_instance_method("CADisplayLink", "setPaused:",                      (void *)&m2_cadl_setPaused);
    oh2_objc_register_instance_method("CADisplayLink", "invalidate",                      (void *)&m2_cadl_invalidate);
    oh2_objc_register_instance_method("CADisplayLink", "timestamp",                       (void *)&m2_cadl_timestamp);
    oh2_objc_register_instance_method("CADisplayLink", "targetTimestamp",                 (void *)&m2_cadl_targetTimestamp);
    oh2_objc_register_instance_method("CADisplayLink", "duration",                        (void *)&m2_cadl_duration);

    /* CAMetalLayer */
    oh2_objc_register_instance_method("CAMetalLayer", "device",                    (void *)&m2_cametal_device);
    oh2_objc_register_instance_method("CAMetalLayer", "setDevice:",                (void *)&m2_cametal_setProp);
    oh2_objc_register_instance_method("CAMetalLayer", "setPixelFormat:",           (void *)&m2_cametal_setProp);
    oh2_objc_register_instance_method("CAMetalLayer", "setFramebufferOnly:",       (void *)&m2_cametal_setProp);
    oh2_objc_register_instance_method("CAMetalLayer", "setDrawableSize:",          (void *)&m2_cametal_setSize);
    oh2_objc_register_instance_method("CAMetalLayer", "setContentsScale:",         (void *)&m2_cametal_setSize);
    oh2_objc_register_instance_method("CAMetalLayer", "setPresentsWithTransaction:", (void *)&m2_cametal_setProp);

    oh2_symbol_register_override("_UIApplicationMain", (void *)&m2_UIApplicationMain);
}

} /* extern "C" */
