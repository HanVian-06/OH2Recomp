/*
 * ui_bridges.cpp — M2 UIKit, QuartzCore (CADisplayLink / CAMetalLayer),
 * and UIApplicationMain bridge with live UE4 MainAppThread -> oh2-ios.pak wait loop.
 */

#include "oh2_runtime.h"
#include "../loader/macho_loader.h"
#include "../libc_wrapper/libc_wrapper.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <unistd.h>

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

struct UIObjPayload {
    void             *isa;
    CGRect64          frame;
    void             *delegate;
    void             *root_vc;
    void             *vc_view;
    void             *layer;
    DisplayLinkState *dl_state;
};

/* UIApplication */
static void *m2_uiapp_sharedApplication(void *cls, const char *sel) {
    (void)cls; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    if (!g_ui_application) {
        g_ui_application = oh2_objc_alloc_instance(oh2_objc_get_class("UIApplication"), sizeof(UIObjPayload));
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
static void *m2_uiview_initWithFrame(void *self, const char *sel, CGRect64 frame) {
    (void)sel;
    if (!self) return nullptr;
    UIObjPayload *p = (UIObjPayload *)self;
    p->frame = frame;
    if (*(void **)self == oh2_objc_get_class("UIWindow")) {
        std::lock_guard<std::mutex> lk(g_ui_mutex);
        g_ui_window = self;
    }
    return self;
}

static CGRect64 m2_uiview_bounds(void *self, const char *sel) {
    (void)sel;
    if (self) {
        UIObjPayload *p = (UIObjPayload *)self;
        if (p->frame.width > 0.0 && p->frame.height > 0.0) {
            return p->frame;
        }
    }
    return {0.0, 0.0, 2340.0, 1080.0};
}

static void m2_uiview_setFrame(void *self, const char *sel, CGRect64 frame) {
    (void)sel;
    if (self) {
        ((UIObjPayload *)self)->frame = frame;
    }
}

static void m2_uiview_addSubview(void *self, const char *sel, void *subview) {
    (void)self; (void)sel; (void)subview;
}

static void m2_uiview_setNeedsDisplay(void *self, const char *sel) {
    (void)self; (void)sel;
}

static void *m2_uiview_layer(void *self, const char *sel) {
    (void)sel;
    if (!self) return nullptr;
    UIObjPayload *p = (UIObjPayload *)self;
    if (!p->layer) {
        p->layer = oh2_objc_alloc_instance(oh2_objc_get_class("CAMetalLayer"), 128);
    }
    return p->layer;
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
    if (self) ((UIObjPayload *)self)->root_vc = vc;
}

static void m2_uiwindow_setScreen(void *self, const char *sel, void *screen) {
    (void)self; (void)sel; (void)screen;
}

/* UIViewController */
static void *m2_uivc_view(void *self, const char *sel) {
    (void)sel;
    if (!self) return nullptr;
    UIObjPayload *p = (UIObjPayload *)self;
    if (!p->vc_view) {
        void *v = oh2_objc_alloc_instance(oh2_objc_get_class("UIView"), sizeof(UIObjPayload));
        ((UIObjPayload *)v)->frame = {0.0, 0.0, 2340.0, 1080.0};
        p->vc_view = v;
    }
    return p->vc_view;
}

static void m2_uivc_setView(void *self, const char *sel, void *v) {
    (void)sel;
    if (self) ((UIObjPayload *)self)->vc_view = v;
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
    if (completion) {
        MinimalBlockHead *b = (MinimalBlockHead *)completion;
        if (b->invoke) b->invoke(completion);
    }
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
        g_ui_screen = oh2_objc_alloc_instance(oh2_objc_get_class("UIScreen"), sizeof(UIObjPayload));
        ((UIObjPayload *)g_ui_screen)->frame = {0.0, 0.0, 2340.0, 1080.0};
    }
    return g_ui_screen;
}

static CGRect64 m2_uiscreen_bounds(void *self, const char *sel) {
    (void)self; (void)sel;
    return {0.0, 0.0, 2340.0, 1080.0};
}

static double m2_uiscreen_scale(void *self, const char *sel) {
    (void)self; (void)sel;
    return 2.0;
}

/* UIDevice */
static void *m2_uidevice_currentDevice(void *cls, const char *sel) {
    (void)cls; (void)sel;
    std::lock_guard<std::mutex> lk(g_ui_mutex);
    if (!g_ui_device) {
        g_ui_device = oh2_objc_alloc_instance(oh2_objc_get_class("UIDevice"), sizeof(UIObjPayload));
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

static void *m2_cadl_displayLinkWithTarget(void *cls, const char *sel, void *target, const char *selector) {
    (void)cls; (void)sel;
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("CADisplayLink"), sizeof(UIObjPayload));
    if (!obj) return nullptr;
    DisplayLinkState *st = new DisplayLinkState();
    st->running.store(1);
    st->paused.store(0);
    st->preferred_fps.store(60);
    st->target   = target;
    st->selector = selector ? oh2_objc_intern_selector(selector) : nullptr;
    st->self_obj = obj;
    ((UIObjPayload *)obj)->dl_state = st;
    return obj;
}

static void m2_cadl_addToRunLoop(void *self, const char *sel, void *runloop, void *mode) {
    (void)sel; (void)runloop; (void)mode;
    if (!self) return;
    DisplayLinkState *st = ((UIObjPayload *)self)->dl_state;
    if (!st || st->thread) return;
    st->running.store(1);
    oh2_darwin_pthread_create(&st->thread, nullptr, cadl_worker, st);
}

static void m2_cadl_setPreferredFPS(void *self, const char *sel, int64_t fps) {
    (void)sel;
    if (!self) return;
    DisplayLinkState *st = ((UIObjPayload *)self)->dl_state;
    if (st) st->preferred_fps.store(fps > 0 ? (int)fps : 60);
}

static void m2_cadl_setPaused(void *self, const char *sel, uint8_t paused) {
    (void)sel;
    if (!self) return;
    DisplayLinkState *st = ((UIObjPayload *)self)->dl_state;
    if (st) st->paused.store(paused ? 1 : 0);
}

static void m2_cadl_invalidate(void *self, const char *sel) {
    (void)sel;
    if (!self) return;
    DisplayLinkState *st = ((UIObjPayload *)self)->dl_state;
    if (!st) return;
    st->running.store(0);
    if (st->thread) {
        pthread_join(st->thread, nullptr);
        st->thread = 0;
    }
}

static double m2_cadl_timestamp(void *self, const char *sel) {
    (void)sel;
    if (!self) return 0.0;
    DisplayLinkState *st = ((UIObjPayload *)self)->dl_state;
    return st ? st->timestamp : 0.0;
}

static double m2_cadl_targetTimestamp(void *self, const char *sel) {
    (void)sel;
    if (!self) return 0.0;
    DisplayLinkState *st = ((UIObjPayload *)self)->dl_state;
    return st ? st->target_timestamp : 0.0;
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

/* M2 UIApplicationMain orchestrating real MainAppThread: until Pak v8 open + index read */
static int m2_UIApplicationMain(int argc, char **argv, void *principalClassName, void *delegateClassName) {
    (void)argc; (void)argv; (void)principalClassName;

    char scratch[128];
    const char *del_name = darwin_cfstring_utf8(delegateClassName, scratch, sizeof(scratch));
    if (!del_name || !del_name[0] || strcmp(del_name, "(null)") == 0 || strcmp(del_name, "<Object>") == 0) {
        del_name = "IOSAppDelegate";
    }
    oh2_log_info("[M2] UIApplicationMain launching delegate '%s'", del_name);

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

extern "C" void oh2_m2_register_ui_bridges(void) {
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

    /* UIWindow */
    oh2_objc_register_instance_method("UIWindow", "initWithFrame:",          (void *)&m2_uiview_initWithFrame);
    oh2_objc_register_instance_method("UIWindow", "makeKeyAndVisible",       (void *)&m2_uiwindow_makeKeyAndVisible);
    oh2_objc_register_instance_method("UIWindow", "rootViewController",      (void *)&m2_uiwindow_rootViewController);
    oh2_objc_register_instance_method("UIWindow", "setRootViewController:",  (void *)&m2_uiwindow_setRootViewController);
    oh2_objc_register_instance_method("UIWindow", "setScreen:",              (void *)&m2_uiwindow_setScreen);

    /* UIView */
    oh2_objc_register_class_method("UIView",       "layerClass",      (void *)&m2_uiview_layerClass);
    oh2_objc_register_instance_method("UIView",    "initWithFrame:",  (void *)&m2_uiview_initWithFrame);
    oh2_objc_register_instance_method("UIView",    "bounds",          (void *)&m2_uiview_bounds);
    oh2_objc_register_instance_method("UIView",    "frame",           (void *)&m2_uiview_bounds);
    oh2_objc_register_instance_method("UIView",    "setFrame:",       (void *)&m2_uiview_setFrame);
    oh2_objc_register_instance_method("UIView",    "addSubview:",     (void *)&m2_uiview_addSubview);
    oh2_objc_register_instance_method("UIView",    "setNeedsDisplay", (void *)&m2_uiview_setNeedsDisplay);
    oh2_objc_register_instance_method("UIView",    "setTag:",         (void *)&m2_uiview_setTag);
    oh2_objc_register_instance_method("UIView",    "layer",           (void *)&m2_uiview_layer);

    /* UIViewController */
    oh2_objc_register_instance_method("UIViewController", "view",                                    (void *)&m2_uivc_view);
    oh2_objc_register_instance_method("UIViewController", "setView:",                                (void *)&m2_uivc_setView);
    oh2_objc_register_instance_method("UIViewController", "viewDidLoad",                             (void *)&m2_uivc_viewDidLoad);
    oh2_objc_register_instance_method("UIViewController", "viewWillAppear:",                         (void *)&m2_uivc_viewWillAppear);
    oh2_objc_register_instance_method("UIViewController", "presentViewController:animated:completion:", (void *)&m2_uivc_presentViewController);

    /* UIImage / UIImageView */
    oh2_objc_register_instance_method("UIImage",     "initWithContentsOfFile:",               (void *)&m2_uiimage_initWithContentsOfFile);
    oh2_objc_register_instance_method("UIImage",     "CGImage",                               (void *)&m2_uiimage_CGImage);
    oh2_objc_register_class_method("UIImage",        "imageWithCGImage:scale:orientation:",   (void *)&m2_uiimage_imageWithCGImage);
    oh2_objc_register_instance_method("UIImageView", "initWithImage:",                        (void *)&m2_uiimageview_initWithImage);

    /* UIScreen */
    oh2_objc_register_class_method("UIScreen",    "mainScreen",   (void *)&m2_uiscreen_mainScreen);
    oh2_objc_register_instance_method("UIScreen", "bounds",       (void *)&m2_uiscreen_bounds);
    oh2_objc_register_instance_method("UIScreen", "nativeBounds", (void *)&m2_uiscreen_bounds);
    oh2_objc_register_instance_method("UIScreen", "scale",        (void *)&m2_uiscreen_scale);
    oh2_objc_register_instance_method("UIScreen", "nativeScale",  (void *)&m2_uiscreen_scale);

    /* UIDevice */
    oh2_objc_register_class_method("UIDevice",    "currentDevice",                          (void *)&m2_uidevice_currentDevice);
    oh2_objc_register_instance_method("UIDevice", "model",                                  (void *)&m2_uidevice_model);
    oh2_objc_register_instance_method("UIDevice", "systemVersion",                          (void *)&m2_uidevice_systemVersion);
    oh2_objc_register_instance_method("UIDevice", "userInterfaceIdiom",                     (void *)&m2_uidevice_userInterfaceIdiom);
    oh2_objc_register_instance_method("UIDevice", "beginGeneratingDeviceOrientationNotifications", (void *)&m2_uidevice_noop);
    oh2_objc_register_instance_method("UIDevice", "setBatteryMonitoringEnabled:",           (void *)&m2_uidevice_setBool);

    /* CADisplayLink */
    oh2_objc_register_class_method("CADisplayLink",    "displayLinkWithTarget:selector:", (void *)&m2_cadl_displayLinkWithTarget);
    oh2_objc_register_instance_method("CADisplayLink", "addToRunLoop:forMode:",          (void *)&m2_cadl_addToRunLoop);
    oh2_objc_register_instance_method("CADisplayLink", "setPreferredFramesPerSecond:",    (void *)&m2_cadl_setPreferredFPS);
    oh2_objc_register_instance_method("CADisplayLink", "setPaused:",                      (void *)&m2_cadl_setPaused);
    oh2_objc_register_instance_method("CADisplayLink", "invalidate",                      (void *)&m2_cadl_invalidate);
    oh2_objc_register_instance_method("CADisplayLink", "timestamp",                       (void *)&m2_cadl_timestamp);
    oh2_objc_register_instance_method("CADisplayLink", "targetTimestamp",                 (void *)&m2_cadl_targetTimestamp);

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
