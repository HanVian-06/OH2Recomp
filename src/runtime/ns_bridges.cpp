/*
 * ns_bridges.cpp — M2 Foundation framework bridges:
 * NSBundle -> /sdcard/Android/data/<pkg>/files/
 * NSFileManager, NSThread (real MainAppThread spawning), NSRunLoop, NSUserDefaults,
 * NSDate, NSNumber, NSData, NSURL, NSArray/NSDictionary (toll-free CF), NSSet, NSString paths.
 */

#include "oh2_runtime.h"
#include "../loader/macho_loader.h"
#include "../libc_wrapper/libc_wrapper.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
void oh2_objc_msgSend(void);
void oh2_dispatch_drain_main_queue(void);
void oh2_perform_selector_on_main_thread(void *target, const char *selector, void *arg, int wait);
}

#define DARWIN_DATE_EPOCH_OFFSET 978307200.0
#define OH2_MAIN_THREAD_MIN_STACK (8U * 1024U * 1024U)

namespace {

static std::mutex g_ns_mutex;
static void *g_ns_main_thread    = nullptr;
static void *g_ns_main_runloop   = nullptr;
static void *g_ns_user_defaults  = nullptr;
static pthread_t g_main_app_tid  = 0;
static std::atomic<int> g_enable_real_main_thread{0};
static std::atomic<int> g_main_app_exited{0};

static thread_local void *tl_current_thread_obj = nullptr;

static std::unordered_map<std::string, void *> g_defaults_map;

struct NSOpaquePayload {
    void        *isa;
    void        *target;
    const char  *selector;
    void        *object;
    size_t       stack_size;
    int64_t      num_i64;
    double       num_f64;
    double       date_ref;
    std::string *str_val;
    std::vector<std::string> *set_items;
};

struct M2TimerEntry {
    void        *timer_obj;
    void        *target;
    const char  *selector;
    void        *user_info;
    double       interval_sec;
    uint64_t     next_fire_ns;
    bool         repeats;
    bool         valid;
};
static std::mutex g_timer_mutex;
static std::vector<M2TimerEntry> g_timers;

static uint64_t mono_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *m2_nstimer_scheduledTimer(void *cls, const char *sel, double interval,
                                       void *target, const char *aSelector,
                                       void *userInfo, uint8_t repeats) {
    (void)cls; (void)sel;
    void *t = oh2_objc_alloc_instance(oh2_objc_get_class("NSTimer"), sizeof(NSOpaquePayload));
    const char *isel = aSelector ? oh2_objc_intern_selector(aSelector) : nullptr;
    if (t) {
        ((NSOpaquePayload *)t)->target   = target;
        ((NSOpaquePayload *)t)->selector = isel;
        ((NSOpaquePayload *)t)->object   = userInfo;
    }
    double clamped = (interval < 0.0) ? 0.0 : interval;
    uint64_t fire_ns = mono_now_ns() + (uint64_t)(clamped * 1e9);
    std::lock_guard<std::mutex> lk(g_timer_mutex);
    g_timers.push_back({t, target, isel, userInfo, clamped, fire_ns, repeats != 0, true});
    return t;
}

static void m2_nstimer_invalidate(void *self, const char *sel) {
    (void)sel;
    if (!self) return;
    std::lock_guard<std::mutex> lk(g_timer_mutex);
    for (auto &te : g_timers) {
        if (te.timer_obj == self) te.valid = false;
    }
}

static void *m2_nstimer_userInfo(void *self, const char *sel) {
    (void)sel;
    return self ? ((NSOpaquePayload *)self)->object : nullptr;
}

static uint8_t m2_nstimer_isValid(void *self, const char *sel) {
    (void)sel;
    if (!self) return 0;
    std::lock_guard<std::mutex> lk(g_timer_mutex);
    for (const auto &te : g_timers) {
        if (te.timer_obj == self) return te.valid ? 1 : 0;
    }
    return 0;
}

static void m2_nsrl_addTimer(void *self, const char *sel, void *timer, void *mode) {
    (void)self; (void)sel; (void)timer; (void)mode;
}

static std::string cfstr_to_std(const void *obj) {
    if (!obj) return "";
    char scratch[1024];
    const char *s = darwin_cfstring_utf8(obj, scratch, sizeof(scratch));
    if (!s || strcmp(s, "(null)") == 0) return "";
    return s;
}

static void recursive_find_ci(const std::string &dir, const std::string &target_name,
                              std::string *out, int depth = 0) {
    if (!out->empty() || depth > 5) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;

    struct dirent *ent;
    std::vector<std::string> subdirs;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        std::string full = dir + "/" + ent->d_name;
        if (strcasecmp(ent->d_name, target_name.c_str()) == 0) {
            *out = full;
            closedir(d);
            return;
        }
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            subdirs.push_back(full);
        }
    }
    closedir(d);

    for (const std::string &sd : subdirs) {
        recursive_find_ci(sd, target_name, out, depth + 1);
        if (!out->empty()) return;
    }
}

/* ============================================================================
 * NSBundle
 * ============================================================================ */
static void *m2_nsbundle_pathForResource(void *self, const char *sel, void *name_cf, void *type_cf) {
    (void)self; (void)sel;
    std::string name = cfstr_to_std(name_cf);
    std::string ext  = cfstr_to_std(type_cf);
    if (name.empty()) return nullptr;
    std::string filename = ext.empty() ? name : (name + "." + ext);

    std::string root = oh2_libc_wrapper_get_bundle_root();
    char direct_buf[1024];
    snprintf(direct_buf, sizeof(direct_buf), "%s/%s", root.c_str(), filename.c_str());
    char translated[1024];
    oh2_libc_wrapper_translate_path(direct_buf, translated, sizeof(translated));

    struct stat st;
    if (stat(translated, &st) == 0) {
        return oh2_objc_create_string(translated);
    }

    std::string found;
    recursive_find_ci(root, filename, &found, 0);
    return found.empty() ? nullptr : oh2_objc_create_string(found.c_str());
}

static void *m2_nsbundle_pathForResourceInDir(void *self, const char *sel,
                                              void *name_cf, void *type_cf, void *subpath_cf) {
    (void)self; (void)sel;
    std::string name = cfstr_to_std(name_cf);
    std::string ext  = cfstr_to_std(type_cf);
    std::string sub  = cfstr_to_std(subpath_cf);
    if (name.empty()) return nullptr;

    std::string filename = ext.empty() ? name : (name + "." + ext);
    std::string base = oh2_libc_wrapper_get_bundle_root();
    if (!sub.empty()) base += "/" + sub;

    char combined[1024], translated[1024];
    snprintf(combined, sizeof(combined), "%s/%s", base.c_str(), filename.c_str());
    oh2_libc_wrapper_translate_path(combined, translated, sizeof(translated));

    struct stat st;
    if (stat(translated, &st) == 0) {
        return oh2_objc_create_string(translated);
    }
    return nullptr;
}

static void *m2_nsbundle_URLForResource(void *self, const char *sel, void *name_cf, void *ext_cf) {
    void *path_ns = m2_nsbundle_pathForResource(self, sel, name_cf, ext_cf);
    if (!path_ns) return nullptr;
    typedef void *(*file_url_t)(void *, const char *, void *);
    void *url_cls = oh2_objc_get_class("NSURL");
    return ((file_url_t)oh2_objc_msgSend)(url_cls, "fileURLWithPath:", path_ns);
}

/* ============================================================================
 * NSFileManager
 * ============================================================================ */
static uint8_t m2_nsfm_fileExistsAtPath(void *self, const char *sel, void *path_cf) {
    (void)self; (void)sel;
    std::string p = cfstr_to_std(path_cf);
    if (p.empty()) return 0;
    char translated[1024];
    oh2_libc_wrapper_translate_path(p.c_str(), translated, sizeof(translated));
    struct stat st;
    return (stat(translated, &st) == 0) ? 1 : 0;
}

static uint8_t m2_nsfm_fileExistsAtPath_isDirectory(void *self, const char *sel, void *path_cf, uint8_t *is_dir) {
    (void)self; (void)sel;
    std::string p = cfstr_to_std(path_cf);
    if (p.empty()) {
        if (is_dir) *is_dir = 0;
        return 0;
    }
    char translated[1024];
    oh2_libc_wrapper_translate_path(p.c_str(), translated, sizeof(translated));
    struct stat st;
    if (stat(translated, &st) != 0) {
        if (is_dir) *is_dir = 0;
        return 0;
    }
    if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return 1;
}

static void *m2_nsfm_contentsOfDirectory(void *self, const char *sel, void *path_cf, void **error_out) {
    (void)self; (void)sel;
    if (error_out) *error_out = nullptr;
    std::string p = cfstr_to_std(path_cf);
    if (p.empty()) return nullptr;
    char translated[1024];
    oh2_libc_wrapper_translate_path(p.c_str(), translated, sizeof(translated));

    DIR *d = opendir(translated);
    if (!d) return nullptr;
    std::vector<void *> items;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        items.push_back(oh2_objc_create_string(ent->d_name));
    }
    closedir(d);
    return oh2_objc_array_create(items.empty() ? nullptr : const_cast<const void **>(items.data()), items.size());
}

static void *m2_nsfm_attributesOfItem(void *self, const char *sel, void *path_cf, void **error_out) {
    (void)self; (void)sel;
    if (error_out) *error_out = nullptr;
    std::string p = cfstr_to_std(path_cf);
    if (p.empty()) return nullptr;
    char translated[1024];
    oh2_libc_wrapper_translate_path(p.c_str(), translated, sizeof(translated));

    struct stat st;
    if (stat(translated, &st) != 0) return nullptr;

    typedef void *(*num_ll_t)(void *, const char *, long long);
    void *num_cls = oh2_objc_get_class("NSNumber");
    void *sz_num  = ((num_ll_t)oh2_objc_msgSend)(num_cls, "numberWithLongLong:", (long long)st.st_size);
    void *dict    = oh2_objc_dict_create();
    oh2_objc_dict_set(dict, "NSFileSize", sz_num);
    return dict;
}

static uint8_t m2_nsfm_createDirectory(void *self, const char *sel, void *path_cf,
                                       uint8_t intermediates, void *attrs, void **error_out) {
    (void)self; (void)sel; (void)attrs;
    if (error_out) *error_out = nullptr;
    std::string p = cfstr_to_std(path_cf);
    if (p.empty()) return 0;
    char translated[1024];
    oh2_libc_wrapper_translate_path(p.c_str(), translated, sizeof(translated));

    if (intermediates) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", translated);
        for (char *q = tmp + 1; *q; ++q) {
            if (*q == '/') {
                *q = '\0';
                mkdir(tmp, 0755);
                *q = '/';
            }
        }
    }
    return (mkdir(translated, 0755) == 0 || errno == EEXIST) ? 1 : 0;
}

static uint8_t m2_nsfm_createDirectoryAtURL(void *self, const char *sel, void *url_obj,
                                            uint8_t intermediates, void *attrs, void **error_out) {
    if (!url_obj) {
        if (error_out) *error_out = nullptr;
        return 0;
    }
    typedef void *(*path_fn_t)(void *, const char *);
    void *path_str = ((path_fn_t)oh2_objc_msgSend)(url_obj, "path");
    return m2_nsfm_createDirectory(self, sel, path_str, intermediates, attrs, error_out);
}

static uint8_t m2_nsfm_removeItem(void *self, const char *sel, void *path_cf, void **error_out) {
    (void)self; (void)sel;
    if (error_out) *error_out = nullptr;
    std::string p = cfstr_to_std(path_cf);
    if (p.empty()) return 0;
    char translated[1024];
    oh2_libc_wrapper_translate_path(p.c_str(), translated, sizeof(translated));
    if (unlink(translated) == 0 || rmdir(translated) == 0) return 1;
    return 0;
}

static void *m2_nsfm_URLsForDirectory(void *self, const char *sel, uint64_t dir, uint64_t mask) {
    (void)self; (void)sel; (void)mask;
    std::string path = oh2_libc_wrapper_get_bundle_root();
    if (dir == 9) { /* NSDocumentDirectory */
        path += "/Documents";
    } else if (dir == 13) { /* NSCachesDirectory */
        path += "/Library/Caches";
    } else if (dir == 14) { /* NSApplicationSupportDirectory */
        path += "/Library/Application Support";
    }
    mkdir(path.c_str(), 0755);

    typedef void *(*file_url_t)(void *, const char *, void *);
    void *url_cls = oh2_objc_get_class("NSURL");
    void *url_obj = ((file_url_t)oh2_objc_msgSend)(url_cls, "fileURLWithPath:", oh2_objc_create_string(path.c_str()));
    const void *arr[1] = { url_obj };
    return oh2_objc_array_create(arr, 1);
}

/* ============================================================================
 * NSThread (real thread execution for OH2's MainAppThread:)
 * ============================================================================ */
struct ThreadStartCtx {
    void       *thread_obj;
    void       *target;
    const char *selector;
    void       *object;
};

static void *nsthread_trampoline(void *raw) {
    ThreadStartCtx *ctx = (ThreadStartCtx *)raw;
    tl_current_thread_obj = ctx->thread_obj;

    bool is_main_app = (ctx->selector && strstr(ctx->selector, "MainAppThread") != nullptr);
    if (is_main_app) {
        oh2_darwin_pthread_setname_np("UE4MainApp");
    } else {
        oh2_darwin_pthread_setname_np("NSThread");
    }

    typedef void (*send_t)(void *, const char *, void *);
    void *resolved_imp = oh2_objc_lookup_imp(ctx->target, ctx->selector);
    oh2_log_info("[M2] nsthread entering selector=%s target=%p imp=%p",
                 ctx->selector ? ctx->selector : "?", ctx->target, resolved_imp);
    ((send_t)oh2_objc_msgSend)(ctx->target, ctx->selector, ctx->object);
    oh2_log_info("[M2] nsthread returned from selector=%s", ctx->selector ? ctx->selector : "?");

    if (is_main_app) {
        oh2_runtime_notify_main_thread_exited();
    }
    delete ctx;
    return nullptr;
}

static void *m2_nsthread_mainThread(void *cls, const char *sel) {
    (void)cls; (void)sel;
    std::lock_guard<std::mutex> lk(g_ns_mutex);
    if (!g_ns_main_thread) {
        g_ns_main_thread = oh2_objc_alloc_instance(oh2_objc_get_class("NSThread"), sizeof(NSOpaquePayload));
        g_main_app_tid = pthread_self();
    }
    return g_ns_main_thread;
}

static void *m2_nsthread_currentThread(void *cls, const char *sel) {
    if (tl_current_thread_obj) return tl_current_thread_obj;
    return m2_nsthread_mainThread(cls, sel);
}

static uint8_t m2_nsthread_isMainThread(void *self, const char *sel) {
    (void)self; (void)sel;
    return (getpid() == gettid()) ? 1 : 0;
}

static void *m2_nsthread_initWithTarget(void *self, const char *sel, void *target, const char *selector, void *arg) {
    (void)sel;
    if (!self) return nullptr;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    p->target     = target;
    p->selector   = selector ? oh2_objc_intern_selector(selector) : nullptr;
    p->object     = arg;
    p->stack_size = OH2_MAIN_THREAD_MIN_STACK;
    return self;
}

static void m2_nsthread_setStackSize(void *self, const char *sel, uint64_t stack_sz) {
    (void)sel;
    if (!self) return;
    ((NSOpaquePayload *)self)->stack_size = (size_t)stack_sz;
}

static void m2_nsthread_setName(void *self, const char *sel, void *name_cf) {
    (void)self; (void)sel; (void)name_cf;
}

static void m2_nsthread_sleepForTimeInterval(void *cls, const char *sel, double secs) {
    (void)cls; (void)sel;
    if (secs <= 0.0) return;
    struct timespec req;
    req.tv_sec  = (time_t)secs;
    req.tv_nsec = (long)((secs - (double)req.tv_sec) * 1e9);
    nanosleep(&req, nullptr);
}

static void spawn_objc_thread(void *thread_obj, void *target, const char *selector, void *arg, size_t req_stack) {
    ThreadStartCtx *ctx = new ThreadStartCtx{thread_obj, target, selector, arg};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stksz = std::max<size_t>(req_stack, OH2_MAIN_THREAD_MIN_STACK);
    pthread_attr_setstacksize(&attr, stksz);
    pthread_t th;
    int rc = pthread_create(&th, &attr, nsthread_trampoline, ctx);
    pthread_attr_destroy(&attr);
    if (rc == 0) {
        pthread_detach(th);
    } else {
        delete ctx;
    }
}

static void m2_nsthread_start(void *self, const char *sel) {
    (void)sel;
    if (!self) return;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (!g_enable_real_main_thread.load()) {
        oh2_log_info("NSThread start deferred (target=%p selector=%s)", p->target, p->selector ? p->selector : "?");
        return;
    }
    if (!p->target || !p->selector) return;
    g_main_app_exited.store(0);
    oh2_log_info("[M2] Starting NSThread on POSIX thread (target=%p selector=%s)", p->target, p->selector);
    spawn_objc_thread(self, p->target, p->selector, p->object, p->stack_size);
}

static void m2_nsthread_detachNewThread(void *cls, const char *sel, const char *selector, void *target, void *obj) {
    (void)cls; (void)sel;
    if (!target || !selector) return;
    void *th_obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSThread"), sizeof(NSOpaquePayload));
    spawn_objc_thread(th_obj, target, oh2_objc_intern_selector(selector), obj, OH2_MAIN_THREAD_MIN_STACK);
}

/* ============================================================================
 * NSRunLoop
 * ============================================================================ */
static void *m2_nsrl_mainRunLoop(void *cls, const char *sel) {
    (void)cls; (void)sel;
    std::lock_guard<std::mutex> lk(g_ns_mutex);
    if (!g_ns_main_runloop) {
        g_ns_main_runloop = oh2_objc_alloc_instance(oh2_objc_get_class("NSRunLoop"), sizeof(NSOpaquePayload));
    }
    return g_ns_main_runloop;
}

static void m2_nsrl_runUntilDate(void *self, const char *sel, void *date) {
    (void)self; (void)sel; (void)date;
    oh2_dispatch_drain_main_queue();
    usleep(16667);
}

/* ============================================================================
 * NSUserDefaults
 * ============================================================================ */
static void *m2_nsud_standard(void *cls, const char *sel) {
    (void)cls; (void)sel;
    std::lock_guard<std::mutex> lk(g_ns_mutex);
    if (!g_ns_user_defaults) {
        g_ns_user_defaults = oh2_objc_alloc_instance(oh2_objc_get_class("NSUserDefaults"), sizeof(NSOpaquePayload));
    }
    return g_ns_user_defaults;
}

static void *m2_nsud_objectForKey(void *self, const char *sel, void *key) {
    (void)self; (void)sel;
    std::string k = cfstr_to_std(key);
    if (k.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(g_ns_mutex);
    auto it = g_defaults_map.find(k);
    return (it != g_defaults_map.end()) ? it->second : nullptr;
}

static void m2_nsud_setObject(void *self, const char *sel, void *val, void *key) {
    (void)self; (void)sel;
    std::string k = cfstr_to_std(key);
    if (k.empty()) return;
    std::lock_guard<std::mutex> lk(g_ns_mutex);
    if (val) g_defaults_map[k] = val;
    else g_defaults_map.erase(k);
}

static uint8_t m2_nsud_boolForKey(void *self, const char *sel, void *key) {
    void *v = m2_nsud_objectForKey(self, sel, key);
    if (!v) return 0;
    typedef int (*int_fn_t)(void *, const char *);
    return ((int_fn_t)oh2_objc_msgSend)(v, "intValue") != 0 ? 1 : 0;
}

static uint8_t m2_nsud_synchronize(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1;
}

/* ============================================================================
 * NSDate
 * ============================================================================ */
static double current_darwin_ref_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((double)ts.tv_sec + (double)ts.tv_nsec * 1e-9) - DARWIN_DATE_EPOCH_OFFSET;
}

static void *m2_nsdate_date(void *cls, const char *sel) {
    (void)cls; (void)sel;
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSDate"), sizeof(NSOpaquePayload));
    if (obj) ((NSOpaquePayload *)obj)->date_ref = current_darwin_ref_secs();
    return obj;
}

static void *m2_nsdate_dateWithTimeIntervalSinceNow(void *cls, const char *sel, double dt) {
    (void)cls; (void)sel;
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSDate"), sizeof(NSOpaquePayload));
    if (obj) ((NSOpaquePayload *)obj)->date_ref = current_darwin_ref_secs() + dt;
    return obj;
}

static double m2_nsdate_timeIntervalSinceRef_cls(void *cls, const char *sel) {
    (void)cls; (void)sel;
    return current_darwin_ref_secs();
}

static double m2_nsdate_timeIntervalSinceRef_inst(void *self, const char *sel) {
    (void)sel;
    if (!self) return current_darwin_ref_secs();
    double r = ((NSOpaquePayload *)self)->date_ref;
    return (r != 0.0) ? r : current_darwin_ref_secs();
}

static double m2_nsdate_timeIntervalSinceNow(void *self, const char *sel) {
    double target = m2_nsdate_timeIntervalSinceRef_inst(self, sel);
    return target - current_darwin_ref_secs();
}

/* ============================================================================
 * NSNumber
 * ============================================================================ */
static void *make_nsnum(int64_t i64, double f64) {
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSNumber"), sizeof(NSOpaquePayload));
    if (obj) {
        ((NSOpaquePayload *)obj)->num_i64 = i64;
        ((NSOpaquePayload *)obj)->num_f64 = f64;
    }
    return obj;
}

static void *m2_nsnum_int(void *cls, const char *sel, int32_t v) { (void)cls; (void)sel; return make_nsnum(v, (double)v); }
static void *m2_nsnum_ll(void *cls, const char *sel, int64_t v)  { (void)cls; (void)sel; return make_nsnum(v, (double)v); }
static void *m2_nsnum_bool(void *cls, const char *sel, uint8_t v){ (void)cls; (void)sel; return make_nsnum(v ? 1 : 0, v ? 1.0 : 0.0); }
static void *m2_nsnum_dbl(void *cls, const char *sel, double v)  { (void)cls; (void)sel; return make_nsnum((int64_t)v, v); }
static int32_t m2_nsnum_intVal(void *s, const char *sel)         { (void)sel; return s ? (int32_t)((NSOpaquePayload *)s)->num_i64 : 0; }
static int64_t m2_nsnum_llVal(void *s, const char *sel)          { (void)sel; return s ? ((NSOpaquePayload *)s)->num_i64 : 0; }
static double  m2_nsnum_dblVal(void *s, const char *sel)         { (void)sel; return s ? ((NSOpaquePayload *)s)->num_f64 : 0.0; }
static uint8_t m2_nsnum_boolVal(void *s, const char *sel)        { (void)sel; return (s && ((NSOpaquePayload *)s)->num_i64 != 0) ? 1 : 0; }

/* ============================================================================
 * NSData (backed by toll-free oh2_objc_data_create / CFData!)
 * ============================================================================ */
static void *m2_nsdata_data(void *cls, const char *sel) {
    (void)cls; (void)sel;
    return oh2_objc_data_create("", 0);
}

static void *m2_nsdata_dataWithBytes(void *cls, const char *sel, const void *bytes, uint64_t len) {
    (void)cls; (void)sel;
    return oh2_objc_data_create(bytes, (size_t)len);
}

static void *m2_nsdata_dataWithContentsOfFile(void *cls, const char *sel, void *path_cf) {
    (void)cls; (void)sel;
    std::string p = cfstr_to_std(path_cf);
    if (p.empty()) return nullptr;
    FILE *fp = oh2_darwin_fopen(p.c_str(), "rb");
    if (!fp) return nullptr;
    oh2_darwin_fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    oh2_darwin_fseek(fp, 0, SEEK_SET);
    std::string buf;
    if (sz > 0) {
        buf.resize((size_t)sz);
        size_t got = oh2_darwin_fread(&buf[0], 1, (size_t)sz, fp);
        buf.resize(got);
    }
    oh2_darwin_fclose(fp);
    return oh2_objc_data_create(buf.data(), buf.size());
}

static void *m2_nsdata_dataWithContentsOpts(void *cls, const char *sel, void *path_cf, uint64_t opts, void **err) {
    (void)opts;
    if (err) *err = nullptr;
    return m2_nsdata_dataWithContentsOfFile(cls, sel, path_cf);
}

static uint64_t m2_nsdata_length(void *self, const char *sel) {
    (void)sel;
    return (uint64_t)oh2_objc_data_length(self);
}

static const void *m2_nsdata_bytes(void *self, const char *sel) {
    (void)sel;
    return oh2_objc_data_bytes(self);
}

/* ============================================================================
 * NSURL
 * ============================================================================ */
static void *m2_nsurl_fileURLWithPath(void *cls, const char *sel, void *path_cf) {
    (void)cls; (void)sel;
    std::string p = cfstr_to_std(path_cf);
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSURL"), sizeof(NSOpaquePayload));
    if (obj) ((NSOpaquePayload *)obj)->str_val = new std::string(p);
    return obj;
}

static void *m2_nsurl_URLWithString(void *cls, const char *sel, void *str_cf) {
    (void)cls; (void)sel;
    std::string s = cfstr_to_std(str_cf);
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSURL"), sizeof(NSOpaquePayload));
    if (obj) ((NSOpaquePayload *)obj)->str_val = new std::string(s);
    return obj;
}

static void *m2_nsurl_path(void *self, const char *sel) {
    (void)sel;
    if (!self) return oh2_objc_create_string("");
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    return oh2_objc_create_string(p->str_val ? p->str_val->c_str() : "");
}

static uint8_t m2_nsurl_setResourceValue(void *self, const char *sel, void *val, void *key, void **error_out) {
    (void)self; (void)sel; (void)val; (void)key;
    if (error_out) *error_out = nullptr;
    return 1;
}

/* ============================================================================
 * NSString extra file / path / prefix / case helpers
 * ============================================================================ */
static void *m2_nsstring_initWithBytes(void *self, const char *sel, const void *bytes,
                                       uint64_t len, uint64_t enc) {
    (void)self; (void)sel;
    if (!bytes || len == 0) return oh2_objc_create_string("");
    bool is_utf16_le = (enc == 0x94000100ULL || enc == 10ULL || enc == 0x90000100ULL);
    if (is_utf16_le) {
        const uint16_t *u16 = (const uint16_t *)bytes;
        size_t chars = (size_t)(len / 2);
        std::string utf8;
        utf8.reserve(chars);
        for (size_t i = 0; i < chars; ++i) {
            uint16_t ch = u16[i];
            if (ch == 0) break;
            if (ch < 0x80) {
                utf8.push_back((char)ch);
            } else if (ch < 0x800) {
                utf8.push_back((char)(0xC0 | (ch >> 6)));
                utf8.push_back((char)(0x80 | (ch & 0x3F)));
            } else {
                utf8.push_back((char)(0xE0 | (ch >> 12)));
                utf8.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
                utf8.push_back((char)(0x80 | (ch & 0x3F)));
            }
        }
        return oh2_objc_create_string_len(utf8.data(), utf8.size());
    }
    return oh2_objc_create_string_len((const char *)bytes, (size_t)len);
}

static void *m2_nsstring_stringWithContentsOfFile_enc(void *cls, const char *sel,
                                                      void *path_cf, uint64_t enc, void **err) {
    (void)cls; (void)sel; (void)enc;
    if (err) *err = nullptr;
    void *d = m2_nsdata_dataWithContentsOfFile(cls, sel, path_cf);
    if (!d) return nullptr;
    const char *bytes = (const char *)oh2_objc_data_bytes(d);
    size_t len = oh2_objc_data_length(d);
    return oh2_objc_create_string_len(bytes ? bytes : "", len);
}

static void *m2_nsstring_initWithContentsOfFile(void *self, const char *sel, void *path_cf) {
    (void)self;
    return m2_nsstring_stringWithContentsOfFile_enc(nullptr, sel, path_cf, 4, nullptr);
}

static void *m2_nsstring_lastPathComponent(void *self, const char *sel) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    size_t pos = s.find_last_of('/');
    std::string res = (pos == std::string::npos) ? s : s.substr(pos + 1);
    return oh2_objc_create_string(res.c_str());
}

static void *m2_nsstring_deletingLast(void *self, const char *sel) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    size_t pos = s.find_last_of('/');
    std::string res = (pos == std::string::npos) ? "" : s.substr(0, pos);
    return oh2_objc_create_string(res.c_str());
}

static void *m2_nsstring_pathExtension(void *self, const char *sel) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    size_t slash = s.find_last_of('/');
    size_t dot   = s.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return oh2_objc_create_string("");
    }
    return oh2_objc_create_string(s.substr(dot + 1).c_str());
}

static void *m2_nsstring_lowercase(void *self, const char *sel) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return oh2_objc_create_string(s.c_str());
}

static void *m2_nsstring_uppercase(void *self, const char *sel) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return oh2_objc_create_string(s.c_str());
}

static uint8_t m2_nsstring_hasPrefix(void *self, const char *sel, void *pref_cf) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    std::string p = cfstr_to_std(pref_cf);
    return (s.size() >= p.size() && s.compare(0, p.size(), p) == 0) ? 1 : 0;
}

static uint8_t m2_nsstring_hasSuffix(void *self, const char *sel, void *suf_cf) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    std::string p = cfstr_to_std(suf_cf);
    return (s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0) ? 1 : 0;
}

static void *m2_nsstring_canonicalMapping(void *self, const char *sel) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    return oh2_objc_create_string(s.c_str());
}

static void *m2_nsstring_initWithCapacity(void *self, const char *sel, uint64_t cap) {
    (void)sel; (void)cap;
    return self;
}

static void *m2_nsobject_performSel0(void *self, const char *sel, const char *target_sel) {
    (void)sel;
    if (!self || !target_sel) return nullptr;
    typedef void *(*fn_t)(void *, const char *);
    return ((fn_t)oh2_objc_msgSend)(self, oh2_objc_intern_selector(target_sel));
}

static void *m2_nsobject_performSel1(void *self, const char *sel, const char *target_sel, void *obj) {
    (void)sel;
    if (!self || !target_sel) return nullptr;
    typedef void *(*fn_t)(void *, const char *, void *);
    return ((fn_t)oh2_objc_msgSend)(self, oh2_objc_intern_selector(target_sel), obj);
}

static void m2_nsobject_performOnMain(void *self, const char *sel, const char *target_sel,
                                      void *arg, uint8_t wait) {
    (void)sel;
    if (!self || !target_sel) return;
    oh2_log_info("[M2] performSelectorOnMainThread selector=%s wait=%u", target_sel, (unsigned)wait);
    oh2_perform_selector_on_main_thread(self, target_sel, arg, wait ? 1 : 0);
}

/* ============================================================================
 * NSArray / NSDictionary / NSSet (toll-free bridged where CF exists)
 * ============================================================================ */
static void *m2_nsarr_arrayWithCapacity(void *cls, const char *sel, uint64_t cap) {
    (void)cls; (void)sel; (void)cap;
    return oh2_objc_array_create(nullptr, 0);
}

static void *m2_nsarr_initWithCapacity(void *self, const char *sel, uint64_t cap) {
    (void)sel; (void)cap;
    typedef void *(*init_t)(void *, const char *);
    return ((init_t)oh2_objc_msgSend)(self, "init");
}

static void *m2_nsdict_dictWithCapacity(void *cls, const char *sel, uint64_t cap) {
    (void)cls; (void)sel; (void)cap;
    return oh2_objc_dict_create();
}

static void *m2_nsdict_initWithCapacity(void *self, const char *sel, uint64_t cap) {
    (void)sel; (void)cap;
    return self;
}

static void *m2_nsarr_arrayWithObject(void *cls, const char *sel, void *obj) {
    (void)cls; (void)sel;
    const void *items[1] = { obj };
    return oh2_objc_array_create(obj ? items : nullptr, obj ? 1 : 0);
}

static void *m2_nsarr_firstObject(void *self, const char *sel) {
    (void)sel;
    return (oh2_objc_array_count(self) > 0) ? oh2_objc_array_get(self, 0) : nullptr;
}

static void *m2_nsarr_lastObject(void *self, const char *sel) {
    (void)sel;
    size_t c = oh2_objc_array_count(self);
    return (c > 0) ? oh2_objc_array_get(self, c - 1) : nullptr;
}

static uint64_t m2_nsdict_count(void *self, const char *sel) {
    (void)sel;
    return (uint64_t)oh2_objc_dict_count(self);
}

static void *m2_nsset_set(void *cls, const char *sel) {
    (void)cls; (void)sel;
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSSet"), sizeof(NSOpaquePayload));
    if (obj) ((NSOpaquePayload *)obj)->set_items = new std::vector<std::string>();
    return obj;
}

static uint64_t m2_nsset_count(void *self, const char *sel) {
    (void)sel;
    if (!self) return 0;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    return p->set_items ? (uint64_t)p->set_items->size() : 0ULL;
}

/* Foundation C functions using bundle root */
static void *m2_NSHomeDirectory(void) {
    return oh2_objc_create_string(oh2_libc_wrapper_get_bundle_root());
}

static void *m2_NSTemporaryDirectory(void) {
    std::string p = std::string(oh2_libc_wrapper_get_bundle_root()) + "/tmp/";
    mkdir(p.c_str(), 0755);
    return oh2_objc_create_string(p.c_str());
}

static void *m2_NSSearchPathForDirectoriesInDomains(uint64_t dir, uint64_t mask, uint8_t expand) {
    (void)mask; (void)expand;
    std::string base = oh2_libc_wrapper_get_bundle_root();
    std::string path = base + "/Documents";
    if (dir == 13) path = base + "/Library/Caches";
    else if (dir == 14) path = base + "/Library/Application Support";
    const void *p = oh2_objc_create_string(path.c_str());
    return oh2_objc_array_create(&p, 1);
}

} // namespace

extern "C" {

void oh2_runtime_set_enable_main_thread(int enable) {
    g_enable_real_main_thread.store(enable ? 1 : 0);
}

void oh2_runtime_notify_main_thread_exited(void) {
    g_main_app_exited.store(1);
}

static void drain_due_timers_on_main_thread(void) {
    uint64_t now = mono_now_ns();
    std::vector<M2TimerEntry> due;
    {
        std::lock_guard<std::mutex> lk(g_timer_mutex);
        for (auto &te : g_timers) {
            if (!te.valid || !te.target || !te.selector) continue;
            if (now >= te.next_fire_ns) {
                due.push_back(te);
                if (te.repeats) {
                    uint64_t step = (uint64_t)((te.interval_sec > 0.001 ? te.interval_sec : 0.016) * 1e9);
                    te.next_fire_ns = now + step;
                } else {
                    te.valid = false;
                }
            }
        }
    }
    static std::unordered_set<std::string> g_logged_timer_sels;
    typedef void (*timer_cb_t)(void *, const char *, void *);
    for (const auto &te : due) {
        if (g_logged_timer_sels.insert(te.selector).second) {
            oh2_log_info("[M2] Firing NSTimer selector=%s target=%p", te.selector, te.target);
        }
        ((timer_cb_t)oh2_objc_msgSend)(te.target, te.selector, te.timer_obj);
    }
}

int oh2_runtime_wait_for_pak_or_boundary(int timeout_ms) {
    int elapsed = 0;
    const int step_ms = 10;
    while (elapsed < timeout_ms) {
        oh2_dispatch_drain_main_queue();
        drain_due_timers_on_main_thread();
        Oh2PakIoTelemetry io;
        oh2_pak_telemetry_get(&io);
        if (io.opened_by_ue4 && io.footer_read && io.index_read_complete &&
            (io.asset_reads_after_index > 0 || oh2_runtime_did_hit_metal_boundary())) {
            return 1;
        }
        if (g_main_app_exited.load()) {
            return (io.opened_by_ue4 && io.index_read_complete) ? 1 : 0;
        }
        usleep(step_ms * 1000);
        elapsed += step_ms;
    }
    Oh2PakIoTelemetry io;
    oh2_pak_telemetry_get(&io);
    return (io.opened_by_ue4 && io.footer_read && io.index_read_complete) ? 1 : 0;
}

void oh2_m2_register_ns_bridges(void) {
    /* NSBundle */
    oh2_objc_register_instance_method("NSBundle", "pathForResource:ofType:",             (void *)&m2_nsbundle_pathForResource);
    oh2_objc_register_instance_method("NSBundle", "pathForResource:ofType:inDirectory:", (void *)&m2_nsbundle_pathForResourceInDir);
    oh2_objc_register_instance_method("NSBundle", "URLForResource:withExtension:",       (void *)&m2_nsbundle_URLForResource);

    /* NSFileManager */
    oh2_objc_register_instance_method("NSFileManager", "fileExistsAtPath:",                  (void *)&m2_nsfm_fileExistsAtPath);
    oh2_objc_register_instance_method("NSFileManager", "fileExistsAtPath:isDirectory:",      (void *)&m2_nsfm_fileExistsAtPath_isDirectory);
    oh2_objc_register_instance_method("NSFileManager", "contentsOfDirectoryAtPath:error:",   (void *)&m2_nsfm_contentsOfDirectory);
    oh2_objc_register_instance_method("NSFileManager", "attributesOfItemAtPath:error:",      (void *)&m2_nsfm_attributesOfItem);
    oh2_objc_register_instance_method("NSFileManager", "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", (void *)&m2_nsfm_createDirectory);
    oh2_objc_register_instance_method("NSFileManager", "createDirectoryAtURL:withIntermediateDirectories:attributes:error:",  (void *)&m2_nsfm_createDirectoryAtURL);
    oh2_objc_register_instance_method("NSFileManager", "removeItemAtPath:error:",            (void *)&m2_nsfm_removeItem);
    oh2_objc_register_instance_method("NSFileManager", "URLsForDirectory:inDomains:",        (void *)&m2_nsfm_URLsForDirectory);

    /* NSThread */
    oh2_objc_register_class_method("NSThread",    "mainThread",                           (void *)&m2_nsthread_mainThread);
    oh2_objc_register_class_method("NSThread",    "currentThread",                        (void *)&m2_nsthread_currentThread);
    oh2_objc_register_class_method("NSThread",    "isMainThread",                         (void *)&m2_nsthread_isMainThread);
    oh2_objc_register_class_method("NSThread",    "detachNewThreadSelector:toTarget:withObject:", (void *)&m2_nsthread_detachNewThread);
    oh2_objc_register_class_method("NSThread",    "sleepForTimeInterval:",                (void *)&m2_nsthread_sleepForTimeInterval);
    oh2_objc_register_instance_method("NSThread", "isMainThread",                         (void *)&m2_nsthread_isMainThread);
    oh2_objc_register_instance_method("NSThread", "initWithTarget:selector:object:",      (void *)&m2_nsthread_initWithTarget);
    oh2_objc_register_instance_method("NSThread", "setStackSize:",                        (void *)&m2_nsthread_setStackSize);
    oh2_objc_register_instance_method("NSThread", "setName:",                             (void *)&m2_nsthread_setName);
    oh2_objc_register_instance_method("NSThread", "start",                                (void *)&m2_nsthread_start);

    /* NSRunLoop */
    oh2_objc_register_class_method("NSRunLoop",       "mainRunLoop",       (void *)&m2_nsrl_mainRunLoop);
    oh2_objc_register_class_method("NSRunLoop",       "currentRunLoop",    (void *)&m2_nsrl_mainRunLoop);
    oh2_objc_register_instance_method("NSRunLoop",    "runUntilDate:",     (void *)&m2_nsrl_runUntilDate);
    oh2_objc_register_instance_method("NSRunLoop",    "addTimer:forMode:", (void *)&m2_nsrl_addTimer);

    /* NSTimer */
    oh2_objc_register_class_method("NSTimer",       "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", (void *)&m2_nstimer_scheduledTimer);
    oh2_objc_register_class_method("NSTimer",       "timerWithTimeInterval:target:selector:userInfo:repeats:",          (void *)&m2_nstimer_scheduledTimer);
    oh2_objc_register_instance_method("NSTimer",    "invalidate",                                                     (void *)&m2_nstimer_invalidate);
    oh2_objc_register_instance_method("NSTimer",    "userInfo",                                                       (void *)&m2_nstimer_userInfo);
    oh2_objc_register_instance_method("NSTimer",    "isValid",                                                        (void *)&m2_nstimer_isValid);

    /* NSUserDefaults */
    oh2_objc_register_class_method("NSUserDefaults",    "standardUserDefaults", (void *)&m2_nsud_standard);
    oh2_objc_register_instance_method("NSUserDefaults", "objectForKey:",        (void *)&m2_nsud_objectForKey);
    oh2_objc_register_instance_method("NSUserDefaults", "stringForKey:",        (void *)&m2_nsud_objectForKey);
    oh2_objc_register_instance_method("NSUserDefaults", "setObject:forKey:",    (void *)&m2_nsud_setObject);
    oh2_objc_register_instance_method("NSUserDefaults", "boolForKey:",          (void *)&m2_nsud_boolForKey);
    oh2_objc_register_instance_method("NSUserDefaults", "synchronize",          (void *)&m2_nsud_synchronize);

    /* NSDate */
    oh2_objc_register_class_method("NSDate",    "date",                            (void *)&m2_nsdate_date);
    oh2_objc_register_class_method("NSDate",    "dateWithTimeIntervalSinceNow:",   (void *)&m2_nsdate_dateWithTimeIntervalSinceNow);
    oh2_objc_register_class_method("NSDate",    "timeIntervalSinceReferenceDate",  (void *)&m2_nsdate_timeIntervalSinceRef_cls);
    oh2_objc_register_instance_method("NSDate", "timeIntervalSinceReferenceDate",  (void *)&m2_nsdate_timeIntervalSinceRef_inst);
    oh2_objc_register_instance_method("NSDate", "timeIntervalSinceNow",            (void *)&m2_nsdate_timeIntervalSinceNow);

    /* NSNumber */
    oh2_objc_register_class_method("NSNumber",    "numberWithInt:",      (void *)&m2_nsnum_int);
    oh2_objc_register_class_method("NSNumber",    "numberWithInteger:",  (void *)&m2_nsnum_ll);
    oh2_objc_register_class_method("NSNumber",    "numberWithLongLong:", (void *)&m2_nsnum_ll);
    oh2_objc_register_class_method("NSNumber",    "numberWithBool:",     (void *)&m2_nsnum_bool);
    oh2_objc_register_class_method("NSNumber",    "numberWithDouble:",   (void *)&m2_nsnum_dbl);
    oh2_objc_register_instance_method("NSNumber", "intValue",            (void *)&m2_nsnum_intVal);
    oh2_objc_register_instance_method("NSNumber", "integerValue",        (void *)&m2_nsnum_llVal);
    oh2_objc_register_instance_method("NSNumber", "longLongValue",       (void *)&m2_nsnum_llVal);
    oh2_objc_register_instance_method("NSNumber", "doubleValue",         (void *)&m2_nsnum_dblVal);
    oh2_objc_register_instance_method("NSNumber", "boolValue",           (void *)&m2_nsnum_boolVal);

    /* NSData */
    oh2_objc_register_class_method("NSData",    "data",                         (void *)&m2_nsdata_data);
    oh2_objc_register_class_method("NSData",    "dataWithBytes:length:",        (void *)&m2_nsdata_dataWithBytes);
    oh2_objc_register_class_method("NSData",    "dataWithContentsOfFile:",      (void *)&m2_nsdata_dataWithContentsOfFile);
    oh2_objc_register_class_method("NSData",    "dataWithContentsOfFile:options:error:", (void *)&m2_nsdata_dataWithContentsOpts);
    oh2_objc_register_instance_method("NSData", "length",                       (void *)&m2_nsdata_length);
    oh2_objc_register_instance_method("NSData", "bytes",                        (void *)&m2_nsdata_bytes);

    /* NSURL */
    oh2_objc_register_class_method("NSURL",    "fileURLWithPath:", (void *)&m2_nsurl_fileURLWithPath);
    oh2_objc_register_class_method("NSURL",    "URLWithString:",   (void *)&m2_nsurl_URLWithString);
    oh2_objc_register_instance_method("NSURL", "path",             (void *)&m2_nsurl_path);
    oh2_objc_register_instance_method("NSURL", "absoluteString",   (void *)&m2_nsurl_path);
    oh2_objc_register_instance_method("NSURL", "setResourceValue:forKey:error:", (void *)&m2_nsurl_setResourceValue);

    /* NSString file/path/string helpers */
    oh2_objc_register_instance_method("NSString",    "initWithBytes:length:encoding:",             (void *)&m2_nsstring_initWithBytes);
    oh2_objc_register_class_method("NSString",       "stringWithContentsOfFile:encoding:error:",     (void *)&m2_nsstring_stringWithContentsOfFile_enc);
    oh2_objc_register_class_method("NSString",    "stringWithContentsOfFile:usedEncoding:error:", (void *)&m2_nsstring_stringWithContentsOfFile_enc);
    oh2_objc_register_instance_method("NSString", "initWithContentsOfFile:",                      (void *)&m2_nsstring_initWithContentsOfFile);
    oh2_objc_register_instance_method("NSString", "initWithContentsOfFile:encoding:error:",       (void *)&m2_nsstring_stringWithContentsOfFile_enc);
    oh2_objc_register_instance_method("NSString", "lastPathComponent",                            (void *)&m2_nsstring_lastPathComponent);
    oh2_objc_register_instance_method("NSString", "stringByDeletingLastPathComponent",            (void *)&m2_nsstring_deletingLast);
    oh2_objc_register_instance_method("NSString", "pathExtension",                                (void *)&m2_nsstring_pathExtension);
    oh2_objc_register_instance_method("NSString", "lowercaseString",                              (void *)&m2_nsstring_lowercase);
    oh2_objc_register_instance_method("NSString", "uppercaseString",                              (void *)&m2_nsstring_uppercase);
    oh2_objc_register_instance_method("NSString", "hasPrefix:",                                   (void *)&m2_nsstring_hasPrefix);
    oh2_objc_register_instance_method("NSString", "hasSuffix:",                                   (void *)&m2_nsstring_hasSuffix);
    oh2_objc_register_instance_method("NSString", "precomposedStringWithCanonicalMapping",        (void *)&m2_nsstring_canonicalMapping);
    oh2_objc_register_instance_method("NSString", "decomposedStringWithCanonicalMapping",         (void *)&m2_nsstring_canonicalMapping);
    oh2_objc_register_instance_method("NSString", "precomposedStringWithCompatibilityMapping",    (void *)&m2_nsstring_canonicalMapping);
    oh2_objc_register_instance_method("NSMutableString", "initWithCapacity:",                     (void *)&m2_nsstring_initWithCapacity);
    oh2_objc_register_instance_method("NSString", "fileSystemRepresentation",                     (void *)darwin_objc_lookup_imp(oh2_objc_create_string(""), "UTF8String"));

    /* NSObject performSelector helpers */
    oh2_objc_register_instance_method("NSObject", "performSelector:",                                            (void *)&m2_nsobject_performSel0);
    oh2_objc_register_instance_method("NSObject", "performSelector:withObject:",                                 (void *)&m2_nsobject_performSel1);
    oh2_objc_register_instance_method("NSObject", "performSelectorOnMainThread:withObject:waitUntilDone:",       (void *)&m2_nsobject_performOnMain);

    /* NSArray / NSDictionary / NSSet extra methods */
    oh2_objc_register_class_method("NSMutableArray",      "arrayWithCapacity:",      (void *)&m2_nsarr_arrayWithCapacity);
    oh2_objc_register_instance_method("NSMutableArray",   "initWithCapacity:",       (void *)&m2_nsarr_initWithCapacity);
    oh2_objc_register_class_method("NSMutableDictionary", "dictionaryWithCapacity:", (void *)&m2_nsdict_dictWithCapacity);
    oh2_objc_register_instance_method("NSMutableDictionary", "initWithCapacity:",    (void *)&m2_nsdict_initWithCapacity);
    oh2_objc_register_class_method("NSArray",             "arrayWithObject:",        (void *)&m2_nsarr_arrayWithObject);
    oh2_objc_register_instance_method("NSArray",      "firstObject",      (void *)&m2_nsarr_firstObject);
    oh2_objc_register_instance_method("NSArray",      "lastObject",       (void *)&m2_nsarr_lastObject);
    oh2_objc_register_instance_method("NSDictionary", "count",            (void *)&m2_nsdict_count);
    oh2_objc_register_class_method("NSSet",           "set",              (void *)&m2_nsset_set);
    oh2_objc_register_instance_method("NSSet",        "count",            (void *)&m2_nsset_count);

    /* Foundation C path helpers mapped under bundle root */
    oh2_symbol_register_override("_NSHomeDirectory",                     (void *)&m2_NSHomeDirectory);
    oh2_symbol_register_override("_NSTemporaryDirectory",                (void *)&m2_NSTemporaryDirectory);
    oh2_symbol_register_override("_NSSearchPathForDirectoriesInDomains", (void *)&m2_NSSearchPathForDirectoriesInDomains);
}

} /* extern "C" */
