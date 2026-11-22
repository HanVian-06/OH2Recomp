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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
    std::vector<void *> *set_objs;
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
    return oh2_is_ios_main_thread() ? 1 : 0;
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

static void m2_nsrl_run(void *self, const char *sel) {
    (void)self; (void)sel;
    while (oh2_runtime_is_live_loop_running()) {
        if (oh2_is_ios_main_thread()) {
            oh2_dispatch_drain_main_queue();
        }
        usleep(16000);
    }
}

static uint8_t m2_nsrl_runModeBeforeDate(void *self, const char *sel, void *mode, void *limitDate) {
    (void)self; (void)sel; (void)mode; (void)limitDate;
    if (oh2_is_ios_main_thread()) {
        oh2_dispatch_drain_main_queue();
    }
    usleep(1000);
    return 1;
}

static void *m2_nsrl_limitDateForMode(void *self, const char *sel, void *mode) {
    (void)self; (void)sel; (void)mode;
    return nullptr;
}

static void *m2_nsrl_currentMode(void *self, const char *sel) {
    (void)self; (void)sel;
    return oh2_objc_create_string("kCFRunLoopDefaultMode");
}

/* ============================================================================
 * NSCondition (real pthread_mutex_t + pthread_cond_t)
 * ============================================================================ */
struct NSConditionState {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    std::string     name;
};
static std::mutex g_nscond_mtx;
static std::unordered_map<void *, NSConditionState *> g_nscond_map;

static NSConditionState *get_or_create_cond(void *obj) {
    if (!obj) return nullptr;
    std::lock_guard<std::mutex> lk(g_nscond_mtx);
    auto it = g_nscond_map.find(obj);
    if (it != g_nscond_map.end()) return it->second;
    auto *st = new NSConditionState();
    pthread_mutex_init(&st->mtx, nullptr);
    pthread_cond_init(&st->cv, nullptr);
    g_nscond_map[obj] = st;
    return st;
}

static void *m2_nscond_init(void *self, const char *sel) {
    (void)sel;
    get_or_create_cond(self);
    return self;
}

static void m2_nscond_dealloc(void *self, const char *sel) {
    (void)sel;
    if (!self) return;
    NSConditionState *st = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_nscond_mtx);
        auto it = g_nscond_map.find(self);
        if (it != g_nscond_map.end()) {
            st = it->second;
            g_nscond_map.erase(it);
        }
    }
    if (st) {
        pthread_cond_destroy(&st->cv);
        pthread_mutex_destroy(&st->mtx);
        delete st;
    }
}

static void m2_nscond_lock(void *self, const char *sel) {
    (void)sel;
    if (auto *st = get_or_create_cond(self)) pthread_mutex_lock(&st->mtx);
}

static void m2_nscond_unlock(void *self, const char *sel) {
    (void)sel;
    if (auto *st = get_or_create_cond(self)) pthread_mutex_unlock(&st->mtx);
}

static void m2_nscond_wait(void *self, const char *sel) {
    (void)sel;
    if (auto *st = get_or_create_cond(self)) pthread_cond_wait(&st->cv, &st->mtx);
}

static uint8_t m2_nscond_waitUntilDate(void *self, const char *sel, void *date_obj) {
    (void)sel;
    auto *st = get_or_create_cond(self);
    if (!st) return 0;
    if (!date_obj) return 0;
    double date_ref = ((NSOpaquePayload *)date_obj)->date_ref;
    if (date_ref == 0.0) {
        typedef double (*time_fn_t)(void *, const char *);
        date_ref = ((time_fn_t)oh2_objc_msgSend)(date_obj, "timeIntervalSinceReferenceDate");
    }
    double posix_sec = date_ref + DARWIN_DATE_EPOCH_OFFSET;
    if (posix_sec <= 0.0) return 0;
    time_t sec = (time_t)posix_sec;
    long nsec = (long)((posix_sec - (double)sec) * 1e9);
    if (nsec < 0) { --sec; nsec += 1000000000L; }
    if (nsec >= 1000000000L) { ++sec; nsec -= 1000000000L; }
    struct timespec ts{sec, nsec};
    int rc = pthread_cond_timedwait(&st->cv, &st->mtx, &ts);
    return (rc == ETIMEDOUT) ? 0 : 1;
}

static void m2_nscond_signal(void *self, const char *sel) {
    (void)sel;
    if (auto *st = get_or_create_cond(self)) pthread_cond_signal(&st->cv);
}

static void m2_nscond_broadcast(void *self, const char *sel) {
    (void)sel;
    if (auto *st = get_or_create_cond(self)) pthread_cond_broadcast(&st->cv);
}

static void *m2_nscond_name(void *self, const char *sel) {
    (void)sel;
    auto *st = get_or_create_cond(self);
    return (st && !st->name.empty()) ? oh2_objc_create_string(st->name.c_str()) : nullptr;
}

static void m2_nscond_setName(void *self, const char *sel, void *name_ns) {
    (void)sel;
    if (auto *st = get_or_create_cond(self)) st->name = cfstr_to_std(name_ns);
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

static uint8_t m2_nsstring_getCString(void *self, const char *sel, char *buf, uint64_t max_len, uint64_t encoding) {
    (void)sel; (void)encoding;
    if (!buf || max_len == 0) return 0;
    std::string s = cfstr_to_std(self);
    if (s.size() + 1 <= max_len) {
        std::memcpy(buf, s.c_str(), s.size() + 1);
        return 1;
    }
    std::memcpy(buf, s.c_str(), (size_t)(max_len - 1));
    buf[max_len - 1] = '\0';
    return 0;
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

static void m2_nsdict_setValueForKey(void *self, const char *sel, void *val, void *key) {
    (void)sel;
    if (!self || !key) return;
    typedef void (*set_fn_t)(void *, const char *, void *, void *);
    ((set_fn_t)oh2_objc_msgSend)(self, "setObject:forKey:", val, key);
}

static void *m2_nsdict_dictionaryWithContentsOfFile(void *cls, const char *sel, void *path_cf) {
    (void)cls; (void)sel;
    std::string p = cfstr_to_std(path_cf);
    if (p.empty()) return nullptr;
    char translated[1024];
    oh2_libc_wrapper_translate_path(p.c_str(), translated, sizeof(translated));
    FILE *fp = std::fopen(translated, "rb");
    if (!fp) return nullptr;
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::string xml;
    if (sz > 0 && sz < 4 * 1024 * 1024) {
        xml.resize((size_t)sz);
        size_t nr = std::fread(&xml[0], 1, (size_t)sz, fp);
        xml.resize(nr);
    }
    std::fclose(fp);
    void *dict = oh2_objc_dict_create();
    if (xml.empty() || !dict) return dict;

    typedef void *(*num_bool_t)(void *, const char *, uint8_t);
    void *num_cls = oh2_objc_get_class("NSNumber");
    size_t pos = 0;
    while ((pos = xml.find("<key>", pos)) != std::string::npos) {
        size_t kstart = pos + 5;
        size_t kend = xml.find("</key>", kstart);
        if (kend == std::string::npos) break;
        std::string key = xml.substr(kstart, kend - kstart);
        size_t vpos = xml.find('<', kend + 6);
        if (vpos == std::string::npos) break;
        if (xml.compare(vpos, 8, "<string>") == 0) {
            size_t vend = xml.find("</string>", vpos + 8);
            if (vend == std::string::npos) break;
            std::string val = xml.substr(vpos + 8, vend - (vpos + 8));
            oh2_objc_dict_set(dict, key.c_str(), oh2_objc_create_string(val.c_str()));
            pos = vend + 9;
        } else if (xml.compare(vpos, 9, "<integer>") == 0) {
            size_t vend = xml.find("</integer>", vpos + 9);
            if (vend == std::string::npos) break;
            std::string val = xml.substr(vpos + 9, vend - (vpos + 9));
            oh2_objc_dict_set(dict, key.c_str(), oh2_objc_create_string(val.c_str()));
            pos = vend + 10;
        } else if (xml.compare(vpos, 7, "<true/>") == 0) {
            void *b = num_cls ? ((num_bool_t)oh2_objc_msgSend)(num_cls, "numberWithBool:", 1) : nullptr;
            if (b) oh2_objc_dict_set(dict, key.c_str(), b);
            pos = vpos + 7;
        } else if (xml.compare(vpos, 8, "<false/>") == 0) {
            void *b = num_cls ? ((num_bool_t)oh2_objc_msgSend)(num_cls, "numberWithBool:", 0) : nullptr;
            if (b) oh2_objc_dict_set(dict, key.c_str(), b);
            pos = vpos + 8;
        } else {
            pos = kend + 6;
        }
    }
    return dict;
}

static void *m2_nsarr_arrayWithObjectsCount(void *cls, const char *sel, const void **objects, uint64_t count) {
    (void)cls; (void)sel;
    return oh2_objc_array_create(objects, (size_t)count);
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

struct DarwinNSFastEnumState {
    uint64_t  state;
    void    **itemsPtr;
    uint64_t *mutationsPtr;
    uint64_t  extra[5];
};

static uint64_t g_fast_enum_mutations_dummy = 0;

static void *m2_nsset_init(void *self, const char *sel) {
    (void)sel;
    if (!self) return nullptr;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (!p->set_objs) p->set_objs = new std::vector<void *>();
    return self;
}

static void *m2_nsset_set(void *cls, const char *sel) {
    (void)cls; (void)sel;
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSSet"), sizeof(NSOpaquePayload));
    if (obj) ((NSOpaquePayload *)obj)->set_objs = new std::vector<void *>();
    return obj;
}

static void *m2_nsset_setWithCapacity(void *cls, const char *sel, uint64_t cap) {
    (void)cap;
    return m2_nsset_set(cls, sel);
}

static void *m2_nsset_setWithObject(void *cls, const char *sel, void *item) {
    (void)cls; (void)sel;
    return oh2_objc_set_create(item ? &item : nullptr, item ? 1 : 0);
}

static void *m2_nsset_setWithArray(void *cls, const char *sel, void *arr) {
    (void)cls; (void)sel;
    size_t n = oh2_objc_array_count(arr);
    std::vector<void *> items;
    items.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        void *it = oh2_objc_array_get(arr, i);
        if (it) items.push_back(it);
    }
    return oh2_objc_set_create(items.data(), items.size());
}

static uint64_t m2_nsset_count(void *self, const char *sel) {
    (void)sel;
    if (!self) return 0;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (p->set_objs) return (uint64_t)p->set_objs->size();
    return p->set_items ? (uint64_t)p->set_items->size() : 0ULL;
}

static void *m2_nsset_allObjects(void *self, const char *sel) {
    (void)sel;
    if (!self) return oh2_objc_array_create(nullptr, 0);
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (!p->set_objs || p->set_objs->empty()) return oh2_objc_array_create(nullptr, 0);
    return oh2_objc_array_create((const void **)p->set_objs->data(), p->set_objs->size());
}

static void *m2_nsset_anyObject(void *self, const char *sel) {
    (void)sel;
    if (!self) return nullptr;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (!p->set_objs || p->set_objs->empty()) return nullptr;
    return (*p->set_objs)[0];
}

static uint8_t m2_nsset_containsObject(void *self, const char *sel, void *item) {
    (void)sel;
    if (!self || !item) return 0;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (!p->set_objs) return 0;
    for (void *v : *p->set_objs) {
        if (v == item) return 1;
    }
    return 0;
}

static void m2_nsset_addObject(void *self, const char *sel, void *item) {
    (void)sel;
    oh2_objc_set_add(self, item);
}

static void m2_nsset_removeObject(void *self, const char *sel, void *item) {
    (void)sel;
    if (!self || !item) return;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (!p->set_objs) return;
    auto &v = *p->set_objs;
    v.erase(std::remove(v.begin(), v.end(), item), v.end());
}

static void m2_nsset_removeAllObjects(void *self, const char *sel) {
    (void)sel;
    if (!self) return;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (p->set_objs) p->set_objs->clear();
}

static uint64_t m2_nsset_countByEnumerating(void *self, const char *sel, DarwinNSFastEnumState *state,
                                            void **stackbuf, uint64_t len) {
    (void)sel;
    if (!self || !state || !stackbuf || len == 0) return 0;
    NSOpaquePayload *p = (NSOpaquePayload *)self;
    if (!p->set_objs || state->state >= p->set_objs->size()) return 0;
    size_t idx = (size_t)state->state;
    size_t avail = p->set_objs->size() - idx;
    size_t n = (avail < (size_t)len) ? avail : (size_t)len;
    for (size_t i = 0; i < n; ++i) {
        stackbuf[i] = (*p->set_objs)[idx + i];
    }
    state->state = (uint64_t)(idx + n);
    state->itemsPtr = stackbuf;
    state->mutationsPtr = &g_fast_enum_mutations_dummy;
    return (uint64_t)n;
}

static uint64_t m2_nsarr_countByEnumerating(void *self, const char *sel, DarwinNSFastEnumState *state,
                                            void **stackbuf, uint64_t len) {
    (void)sel;
    if (!self || !state || !stackbuf || len == 0) return 0;
    size_t total = oh2_objc_array_count(self);
    if (state->state >= total) return 0;
    size_t idx = (size_t)state->state;
    size_t avail = total - idx;
    size_t n = (avail < (size_t)len) ? avail : (size_t)len;
    for (size_t i = 0; i < n; ++i) {
        stackbuf[i] = oh2_objc_array_get(self, idx + i);
    }
    state->state = (uint64_t)(idx + n);
    state->itemsPtr = stackbuf;
    state->mutationsPtr = &g_fast_enum_mutations_dummy;
    return (uint64_t)n;
}

static void *m2_nsdata_dataWithBytesNoCopy(void *cls, const char *sel, const void *bytes,
                                           uint64_t len, uint8_t freeWhenDone) {
    (void)cls; (void)sel; (void)freeWhenDone;
    return oh2_objc_data_create(bytes, (size_t)len);
}

static void *m2_nsdata_initWithBytes(void *self, const char *sel, const void *bytes, uint64_t len) {
    (void)self; (void)sel;
    return oh2_objc_data_create(bytes, (size_t)len);
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

static std::atomic<int> g_ios_main_set{0};
static pthread_t        g_ios_main_pthread{};

extern "C" {

void oh2_set_ios_main_thread(void) {
    g_ios_main_pthread = pthread_self();
    g_ios_main_set.store(1);
}

int oh2_is_ios_main_thread(void) {
    if (g_ios_main_set.load()) {
        return pthread_equal(pthread_self(), g_ios_main_pthread) ? 1 : 0;
    }
    return (getpid() == gettid()) ? 1 : 0;
}

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

void oh2_runtime_drain_timers(void) {
    drain_due_timers_on_main_thread();
}

void *oh2_objc_set_create(void *const *items, size_t count) {
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSSet"), sizeof(NSOpaquePayload));
    if (!obj) return nullptr;
    auto *vec = new std::vector<void *>();
    if (items && count > 0) {
        vec->assign(items, items + count);
    }
    ((NSOpaquePayload *)obj)->set_objs = vec;
    return obj;
}

size_t oh2_objc_set_count(const void *set_obj) {
    if (!set_obj) return 0;
    const NSOpaquePayload *p = (const NSOpaquePayload *)set_obj;
    return p->set_objs ? p->set_objs->size() : 0;
}

void *oh2_objc_set_get(const void *set_obj, size_t idx) {
    if (!set_obj) return nullptr;
    const NSOpaquePayload *p = (const NSOpaquePayload *)set_obj;
    if (!p->set_objs || idx >= p->set_objs->size()) return nullptr;
    return (*p->set_objs)[idx];
}

void oh2_objc_set_add(void *set_obj, void *item) {
    if (!set_obj || !item) return;
    NSOpaquePayload *p = (NSOpaquePayload *)set_obj;
    if (!p->set_objs) p->set_objs = new std::vector<void *>();
    for (void *v : *p->set_objs) {
        if (v == item) return;
    }
    p->set_objs->push_back(item);
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

/* ============================================================================
 * M5 Batch Foundation Additions
 * ============================================================================ */
static std::mutex g_mdata_mtx;
static std::unordered_map<void *, std::vector<uint8_t> *> g_mdata_map;

static std::vector<uint8_t> *get_mdata(void *obj, bool create) {
    if (!obj) return nullptr;
    std::lock_guard<std::mutex> lk(g_mdata_mtx);
    auto it = g_mdata_map.find(obj);
    if (it != g_mdata_map.end()) return it->second;
    if (!create) return nullptr;
    auto *v = new std::vector<uint8_t>();
    g_mdata_map[obj] = v;
    return v;
}

static void *m5_nsdate_since1970(void *cls, const char *sel, double sec1970) {
    (void)cls; (void)sel;
    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSDate"), sizeof(NSOpaquePayload));
    if (obj) ((NSOpaquePayload *)obj)->date_ref = sec1970 - DARWIN_DATE_EPOCH_OFFSET;
    return obj;
}

static double m5_nsdate_timeIntervalSince1970(void *self, const char *sel) {
    return m2_nsdate_timeIntervalSinceRef_inst(self, sel) + DARWIN_DATE_EPOCH_OFFSET;
}

static void *m5_nsnum_ulong(void *cls, const char *sel, unsigned long v) {
    (void)cls; (void)sel; return make_nsnum((int64_t)v, (double)v);
}
static void *m5_nsnum_ull(void *cls, const char *sel, unsigned long long v) {
    (void)cls; (void)sel; return make_nsnum((int64_t)v, (double)v);
}
static void *m5_nsnum_float(void *cls, const char *sel, float v) {
    (void)cls; (void)sel; return make_nsnum((int64_t)v, (double)v);
}
static unsigned long long m5_nsnum_ullVal(void *self, const char *sel) {
    (void)sel; return self ? (unsigned long long)((NSOpaquePayload *)self)->num_i64 : 0ULL;
}
static void *m5_nsnum_stringValue(void *self, const char *sel) {
    (void)sel;
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", self ? (long long)((NSOpaquePayload *)self)->num_i64 : 0LL);
    return oh2_objc_create_string(buf);
}

static void *m5_nsmdata_initWithLen(void *self, const char *sel, uint64_t len) {
    (void)sel;
    if (auto *v = get_mdata(self, true)) v->assign((size_t)len, 0);
    return self;
}
static void *m5_nsmdata_mutableBytes(void *self, const char *sel) {
    (void)sel;
    auto *v = get_mdata(self, true);
    if (!v) return nullptr;
    if (v->empty()) v->resize(1, 0);
    return v->data();
}
static uint64_t m5_nsmdata_length(void *self, const char *sel) {
    (void)sel;
    if (auto *v = get_mdata(self, false)) return v->size();
    return (uint64_t)oh2_objc_data_length(self);
}
static const void *m5_nsmdata_bytes(void *self, const char *sel) {
    (void)sel;
    if (auto *v = get_mdata(self, false)) return v->data();
    return oh2_objc_data_bytes(self);
}
static void m5_nsmdata_appendBytes(void *self, const char *sel, const void *bytes, uint64_t len) {
    (void)sel;
    if (!bytes || len == 0) return;
    if (auto *v = get_mdata(self, true)) {
        const uint8_t *p = (const uint8_t *)bytes;
        v->insert(v->end(), p, p + (size_t)len);
    }
}
static void m5_nsmdata_appendData(void *self, const char *sel, void *other) {
    (void)sel;
    if (!other) return;
    uint64_t len = m5_nsmdata_length(other, "length");
    const void *b = m5_nsmdata_bytes(other, "bytes");
    m5_nsmdata_appendBytes(self, "appendBytes:length:", b, len);
}
static void *m5_nsdata_dataWithURL(void *cls, const char *sel, void *url) {
    void *path = m2_nsurl_path(url, "path");
    return m2_nsdata_dataWithContentsOfFile(cls, sel, path);
}
static void *m5_nsdata_base64(void *self, const char *sel, uint64_t opts) {
    (void)sel; (void)opts;
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint64_t len = m5_nsmdata_length(self, "length");
    const uint8_t *in = (const uint8_t *)m5_nsmdata_bytes(self, "bytes");
    if (!in || len == 0) return oh2_objc_create_string("");
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (uint64_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)in[i]) << 16;
        if (i + 1 < len) v |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < len) v |= ((uint32_t)in[i + 2]);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back((i + 1 < len) ? tbl[(v >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? tbl[v & 63] : '=');
    }
    return oh2_objc_create_string(out.c_str());
}

struct M5NSRange { uint64_t location; uint64_t length; };
static M5NSRange m5_nsstr_rangeOfString(void *self, const char *sel, void *sub_ns) {
    (void)sel;
    std::string a = cfstr_to_std(self);
    std::string b = cfstr_to_std(sub_ns);
    if (b.empty()) return M5NSRange{UINT64_MAX, 0};
    size_t pos = a.find(b);
    if (pos == std::string::npos) return M5NSRange{UINT64_MAX, 0};
    return M5NSRange{pos, b.size()};
}
static void *m5_nsstr_substringFrom(void *self, const char *sel, uint64_t idx) {
    (void)sel;
    std::string a = cfstr_to_std(self);
    if (idx >= a.size()) return oh2_objc_create_string("");
    return oh2_objc_create_string(a.substr((size_t)idx).c_str());
}
static void *m5_nsstr_replaceOcc(void *self, const char *sel, void *tgt_ns, void *rep_ns) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    std::string t = cfstr_to_std(tgt_ns);
    std::string r = cfstr_to_std(rep_ns);
    if (!t.empty()) {
        size_t pos = 0;
        while ((pos = s.find(t, pos)) != std::string::npos) {
            s.replace(pos, t.size(), r);
            pos += r.size();
        }
    }
    return oh2_objc_create_string(s.c_str());
}
static void *m5_nsstr_dataUsingEncoding(void *self, const char *sel, uint64_t enc, uint8_t lossy) {
    (void)sel; (void)enc; (void)lossy;
    std::string s = cfstr_to_std(self);
    return oh2_objc_data_create(s.data(), s.size());
}
static void *m5_nsstr_initWithDataEnc(void *self, const char *sel, void *data, uint64_t enc) {
    (void)self; (void)sel; (void)enc;
    const void *b = m5_nsmdata_bytes(data, "bytes");
    uint64_t len = m5_nsmdata_length(data, "length");
    if (!b || len == 0) return oh2_objc_create_string("");
    return oh2_objc_create_string_len((const char *)b, (size_t)len);
}
static uint16_t m5_nsstr_charAtIdx(void *self, const char *sel, uint64_t idx) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    return (idx < s.size()) ? (uint16_t)(uint8_t)s[(size_t)idx] : 0;
}
static int64_t m5_nsstr_compare(void *self, const char *sel, void *other) {
    (void)sel;
    std::string a = cfstr_to_std(self);
    std::string b = cfstr_to_std(other);
    int c = a.compare(b);
    return (c < 0) ? -1 : (c > 0 ? 1 : 0);
}
static void *m5_nsstr_delPathExt(void *self, const char *sel) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    size_t slash = s.find_last_of('/');
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        s.resize(dot);
    }
    return oh2_objc_create_string(s.c_str());
}
static void *m5_nsstr_componentsSep(void *self, const char *sel, void *sep_ns) {
    (void)sel;
    std::string s = cfstr_to_std(self);
    std::string sep = cfstr_to_std(sep_ns);
    void *arr = oh2_objc_array_create(nullptr, 0);
    if (sep.empty()) {
        oh2_objc_array_add(arr, oh2_objc_create_string(s.c_str()));
        return arr;
    }
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            oh2_objc_array_add(arr, oh2_objc_create_string(s.substr(start).c_str()));
            break;
        }
        oh2_objc_array_add(arr, oh2_objc_create_string(s.substr(start, pos - start).c_str()));
        start = pos + sep.size();
    }
    return arr;
}
static void *m5_nsstr_percentEncode(void *self, const char *sel, void *cset) {
    (void)sel; (void)cset;
    std::string s = cfstr_to_std(self);
    return oh2_objc_create_string(s.c_str());
}
static uint8_t m5_nsstr_writeToFile(void *self, const char *sel, void *path_cf, uint8_t atom, uint64_t enc, void **err) {
    (void)sel; (void)atom; (void)enc;
    if (err) *err = nullptr;
    std::string p = cfstr_to_std(path_cf);
    std::string s = cfstr_to_std(self);
    FILE *fp = oh2_darwin_fopen(p.c_str(), "wb");
    if (!fp) return 0;
    size_t w = fwrite(s.data(), 1, s.size(), fp);
    oh2_darwin_fclose(fp);
    return (w == s.size()) ? 1 : 0;
}

static void *m5_nsbundle_localizedString(void *self, const char *sel, void *key, void *val, void *tbl) {
    (void)self; (void)sel; (void)tbl;
    std::string v = cfstr_to_std(val);
    if (!v.empty()) return val;
    return key ? key : oh2_objc_create_string("");
}
static void *m5_nsbundle_bundleURL(void *self, const char *sel) {
    (void)self;
    return m2_nsurl_fileURLWithPath(oh2_objc_get_class("NSURL"), sel,
                                    oh2_objc_create_string(oh2_objc_get_bundle_root()));
}
static void *m5_nsbundle_privFw(void *self, const char *sel) {
    (void)self; (void)sel;
    return oh2_objc_create_string(oh2_objc_get_bundle_root());
}

static void *m5_nsfm_fsAttrs(void *self, const char *sel, void *path_cf, void **err) {
    (void)self; (void)sel;
    if (err) *err = nullptr;
    std::string p = cfstr_to_std(path_cf);
    char mapped[1024];
    const char *use_p = p.empty() ? oh2_objc_get_bundle_root() :
                        (oh2_libc_wrapper_translate_path(p.c_str(), mapped, sizeof(mapped)) == 0 ? mapped : p.c_str());
    struct statvfs stv{};
    uint64_t free_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    uint64_t total_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
    if (statvfs(use_p, &stv) == 0 && stv.f_frsize > 0) {
        free_bytes = (uint64_t)stv.f_bavail * (uint64_t)stv.f_frsize;
        total_bytes = (uint64_t)stv.f_blocks * (uint64_t)stv.f_frsize;
    }
    void *d = oh2_objc_dict_create();
    oh2_objc_dict_set(d, "NSFileSystemFreeSize", make_nsnum((int64_t)free_bytes, (double)free_bytes));
    oh2_objc_dict_set(d, "NSFileSystemSize", make_nsnum((int64_t)total_bytes, (double)total_bytes));
    return d;
}
static uint8_t m5_nsfm_moveURL(void *self, const char *sel, void *src_url, void *dst_url, void **err) {
    (void)self; (void)sel;
    if (err) *err = nullptr;
    std::string s = cfstr_to_std(m2_nsurl_path(src_url, "path"));
    std::string d = cfstr_to_std(m2_nsurl_path(dst_url, "path"));
    return (oh2_darwin_rename(s.c_str(), d.c_str()) == 0) ? 1 : 0;
}

static void *m5_nsdict_dictWithObjKey(void *cls, const char *sel, void *obj, void *key) {
    (void)cls; (void)sel;
    void *d = oh2_objc_dict_create();
    std::string k = cfstr_to_std(key);
    if (!k.empty() && obj) oh2_objc_dict_set(d, k.c_str(), obj);
    return d;
}
static void *m5_nsdict_dictWithObjsKeysCount(void *cls, const char *sel, void *const *objs, void *const *keys, uint64_t cnt) {
    (void)cls; (void)sel;
    void *d = oh2_objc_dict_create();
    for (uint64_t i = 0; i < cnt && objs && keys; ++i) {
        std::string k = cfstr_to_std(keys[i]);
        if (!k.empty() && objs[i]) oh2_objc_dict_set(d, k.c_str(), objs[i]);
    }
    return d;
}
static void *m5_nsdict_subscript(void *self, const char *sel, void *key) {
    (void)sel;
    std::string k = cfstr_to_std(key);
    return (!k.empty()) ? oh2_objc_dict_get(self, k.c_str()) : nullptr;
}
static void m5_nsdict_setSubscript(void *self, const char *sel, void *obj, void *key) {
    (void)sel;
    std::string k = cfstr_to_std(key);
    if (!k.empty() && obj) oh2_objc_dict_set(self, k.c_str(), obj);
}
static void *m5_nsdict_allKeys(void *self, const char *sel) {
    (void)self; (void)sel;
    return oh2_objc_array_create(nullptr, 0);
}
static void *m5_nsarr_initWithObjsCount(void *self, const char *sel, const void **objs, uint64_t cnt) {
    (void)self; (void)sel;
    return oh2_objc_array_create(objs, (size_t)cnt);
}
static uint64_t m5_nsarr_indexOfObject(void *self, const char *sel, void *obj) {
    (void)sel;
    size_t n = oh2_objc_array_count(self);
    for (size_t i = 0; i < n; ++i) {
        if (oh2_objc_array_get(self, i) == obj) return (uint64_t)i;
    }
    return UINT64_MAX;
}
static void *m5_nserr_errorWithDomain(void *cls, const char *sel, void *dom, int64_t code, void *ui) {
    (void)sel; (void)dom; (void)ui;
    void *e = oh2_objc_alloc_instance(cls, sizeof(NSOpaquePayload));
    if (e) ((NSOpaquePayload *)e)->num_i64 = code;
    return e;
}
static int64_t m5_nserr_code(void *self, const char *sel) {
    (void)sel;
    return self ? ((NSOpaquePayload *)self)->num_i64 : 0;
}
static void *m5_ns_ret_str_oh2(void *s, const char *sel) { (void)s; (void)sel; return oh2_objc_create_string("OH2"); }
static int64_t m5_ns_ret_pid(void *s, const char *sel) { (void)s; (void)sel; return (int64_t)getpid(); }
static uint8_t m5_ns_ret_no(void *s, const char *sel) { (void)s; (void)sel; return 0; }
static uint8_t m5_ns_ret_yes(void *s, const char *sel) { (void)s; (void)sel; return 1; }
static void *m5_ns_ret_nil(void *s, const char *sel) { (void)s; (void)sel; return nullptr; }
static void *m5_ns_ret_self(void *s, const char *sel) { (void)sel; return s; }
static void m5_ns_noop(void *s, const char *sel) { (void)s; (void)sel; }
static void *m5_ns_singleton(void *c, const char *sel) { (void)sel; return oh2_objc_alloc_instance(c, 64); }
static void *m5_ns_prefLangs(void *c, const char *sel) {
    (void)c; (void)sel;
    const void *en = oh2_objc_create_string("en-US");
    return oh2_objc_array_create(&en, 1);
}
static void *m5_nsbr_beginAccess(void *self, const char *sel, void *handler) {
    (void)self; (void)sel;
    if (handler) {
        struct Blk1 { void *isa; int32_t f, r; void (*inv)(void *, void *); };
        auto *b = (Blk1 *)handler;
        if (b->inv) b->inv(handler, nullptr);
    }
    return nullptr;
}
static void *m5_nsop_mainQueue(void *cls, const char *sel) {
    (void)sel;
    static void *mq = nullptr;
    if (!mq) mq = oh2_objc_alloc_instance(cls, 64);
    return mq;
}
static void m5_nsop_addBlock(void *self, const char *sel, void *blk) {
    (void)self; (void)sel;
    if (blk) {
        struct Blk0 { void *isa; int32_t f, r; void (*inv)(void *); };
        auto *b = (Blk0 *)blk;
        if (b->inv) b->inv(blk);
    }
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
    oh2_objc_register_instance_method("NSRunLoop",    "run",               (void *)&m2_nsrl_run);
    oh2_objc_register_instance_method("NSRunLoop",    "runUntilDate:",     (void *)&m2_nsrl_runUntilDate);
    oh2_objc_register_instance_method("NSRunLoop",    "runMode:beforeDate:", (void *)&m2_nsrl_runModeBeforeDate);
    oh2_objc_register_instance_method("NSRunLoop",    "limitDateForMode:", (void *)&m2_nsrl_limitDateForMode);
    oh2_objc_register_instance_method("NSRunLoop",    "currentMode",       (void *)&m2_nsrl_currentMode);
    oh2_objc_register_instance_method("NSRunLoop",    "addTimer:forMode:", (void *)&m2_nsrl_addTimer);

    /* NSCondition */
    oh2_objc_register_instance_method("NSCondition",  "init",              (void *)&m2_nscond_init);
    oh2_objc_register_instance_method("NSCondition",  "dealloc",           (void *)&m2_nscond_dealloc);
    oh2_objc_register_instance_method("NSCondition",  "lock",              (void *)&m2_nscond_lock);
    oh2_objc_register_instance_method("NSCondition",  "unlock",            (void *)&m2_nscond_unlock);
    oh2_objc_register_instance_method("NSCondition",  "wait",              (void *)&m2_nscond_wait);
    oh2_objc_register_instance_method("NSCondition",  "waitUntilDate:",    (void *)&m2_nscond_waitUntilDate);
    oh2_objc_register_instance_method("NSCondition",  "signal",            (void *)&m2_nscond_signal);
    oh2_objc_register_instance_method("NSCondition",  "broadcast",         (void *)&m2_nscond_broadcast);
    oh2_objc_register_instance_method("NSCondition",  "name",              (void *)&m2_nscond_name);
    oh2_objc_register_instance_method("NSCondition",  "setName:",          (void *)&m2_nscond_setName);

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
    oh2_objc_register_class_method("NSData",    "dataWithBytesNoCopy:length:freeWhenDone:", (void *)&m2_nsdata_dataWithBytesNoCopy);
    oh2_objc_register_class_method("NSData",    "dataWithContentsOfFile:",      (void *)&m2_nsdata_dataWithContentsOfFile);
    oh2_objc_register_class_method("NSData",    "dataWithContentsOfFile:options:error:", (void *)&m2_nsdata_dataWithContentsOpts);
    oh2_objc_register_instance_method("NSData", "initWithBytes:length:",        (void *)&m2_nsdata_initWithBytes);
    oh2_objc_register_instance_method("NSData", "initWithBytesNoCopy:length:freeWhenDone:", (void *)&m2_nsdata_initWithBytes);
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
    oh2_objc_register_instance_method("NSString", "getCString:maxLength:encoding:",               (void *)&m2_nsstring_getCString);
    oh2_objc_register_instance_method("NSString", "fileSystemRepresentation",                     (void *)darwin_objc_lookup_imp(oh2_objc_create_string(""), "UTF8String"));

    /* NSObject performSelector helpers */
    oh2_objc_register_instance_method("NSObject", "performSelector:",                                            (void *)&m2_nsobject_performSel0);
    oh2_objc_register_instance_method("NSObject", "performSelector:withObject:",                                 (void *)&m2_nsobject_performSel1);
    oh2_objc_register_instance_method("NSObject", "performSelectorOnMainThread:withObject:waitUntilDone:",       (void *)&m2_nsobject_performOnMain);

    /* NSArray / NSDictionary / NSSet extra methods */
    oh2_objc_register_class_method("NSMutableArray",      "arrayWithCapacity:",      (void *)&m2_nsarr_arrayWithCapacity);
    oh2_objc_register_instance_method("NSMutableArray",   "initWithCapacity:",       (void *)&m2_nsarr_initWithCapacity);
    oh2_objc_register_class_method("NSDictionary",        "dictionaryWithContentsOfFile:", (void *)&m2_nsdict_dictionaryWithContentsOfFile);
    oh2_objc_register_class_method("NSMutableDictionary", "dictionaryWithContentsOfFile:", (void *)&m2_nsdict_dictionaryWithContentsOfFile);
    oh2_objc_register_class_method("NSMutableDictionary", "dictionaryWithCapacity:", (void *)&m2_nsdict_dictWithCapacity);
    oh2_objc_register_instance_method("NSMutableDictionary", "initWithCapacity:",    (void *)&m2_nsdict_initWithCapacity);
    oh2_objc_register_instance_method("NSMutableDictionary", "setValue:forKey:",     (void *)&m2_nsdict_setValueForKey);
    oh2_objc_register_class_method("NSArray",             "arrayWithObject:",        (void *)&m2_nsarr_arrayWithObject);
    oh2_objc_register_class_method("NSArray",             "arrayWithObjects:count:", (void *)&m2_nsarr_arrayWithObjectsCount);
    oh2_objc_register_instance_method("NSArray",      "firstObject",      (void *)&m2_nsarr_firstObject);
    oh2_objc_register_instance_method("NSArray",      "lastObject",       (void *)&m2_nsarr_lastObject);
    oh2_objc_register_instance_method("NSArray",      "countByEnumeratingWithState:objects:count:", (void *)&m2_nsarr_countByEnumerating);
    oh2_objc_register_instance_method("NSDictionary", "count",            (void *)&m2_nsdict_count);
    oh2_objc_register_class_method("NSSet",           "set",              (void *)&m2_nsset_set);
    oh2_objc_register_class_method("NSSet",           "setWithObject:",   (void *)&m2_nsset_setWithObject);
    oh2_objc_register_class_method("NSSet",           "setWithArray:",    (void *)&m2_nsset_setWithArray);
    oh2_objc_register_instance_method("NSSet",        "init",             (void *)&m2_nsset_init);
    oh2_objc_register_instance_method("NSSet",        "count",            (void *)&m2_nsset_count);
    oh2_objc_register_instance_method("NSSet",        "allObjects",       (void *)&m2_nsset_allObjects);
    oh2_objc_register_instance_method("NSSet",        "anyObject",        (void *)&m2_nsset_anyObject);
    oh2_objc_register_instance_method("NSSet",        "containsObject:",  (void *)&m2_nsset_containsObject);
    oh2_objc_register_instance_method("NSSet",        "countByEnumeratingWithState:objects:count:", (void *)&m2_nsset_countByEnumerating);
    oh2_objc_register_class_method("NSMutableSet",    "set",              (void *)&m2_nsset_set);
    oh2_objc_register_class_method("NSMutableSet",    "setWithCapacity:", (void *)&m2_nsset_setWithCapacity);
    oh2_objc_register_class_method("NSMutableSet",    "setWithObject:",   (void *)&m2_nsset_setWithObject);
    oh2_objc_register_class_method("NSMutableSet",    "setWithArray:",    (void *)&m2_nsset_setWithArray);
    oh2_objc_register_instance_method("NSMutableSet", "init",             (void *)&m2_nsset_init);
    oh2_objc_register_instance_method("NSMutableSet", "count",            (void *)&m2_nsset_count);
    oh2_objc_register_instance_method("NSMutableSet", "allObjects",       (void *)&m2_nsset_allObjects);
    oh2_objc_register_instance_method("NSMutableSet", "anyObject",        (void *)&m2_nsset_anyObject);
    oh2_objc_register_instance_method("NSMutableSet", "addObject:",       (void *)&m2_nsset_addObject);
    oh2_objc_register_instance_method("NSMutableSet", "removeObject:",    (void *)&m2_nsset_removeObject);
    oh2_objc_register_instance_method("NSMutableSet", "removeAllObjects", (void *)&m2_nsset_removeAllObjects);
    oh2_objc_register_instance_method("NSMutableSet", "containsObject:",  (void *)&m2_nsset_containsObject);
    oh2_objc_register_instance_method("NSMutableSet", "countByEnumeratingWithState:objects:count:", (void *)&m2_nsset_countByEnumerating);

    /* Foundation C path helpers mapped under bundle root */
    oh2_symbol_register_override("_NSHomeDirectory",                     (void *)&m2_NSHomeDirectory);
    oh2_symbol_register_override("_NSTemporaryDirectory",                (void *)&m2_NSTemporaryDirectory);
    oh2_symbol_register_override("_NSSearchPathForDirectoriesInDomains", (void *)&m2_NSSearchPathForDirectoriesInDomains);

    /* M5 Batch Foundation Registrations */
    oh2_objc_register_class_method("NSDate",    "dateWithTimeIntervalSince1970:", (void *)&m5_nsdate_since1970);
    oh2_objc_register_instance_method("NSDate", "timeIntervalSince1970",          (void *)&m5_nsdate_timeIntervalSince1970);

    oh2_objc_register_class_method("NSNumber",    "numberWithUnsignedLong:",     (void *)&m5_nsnum_ulong);
    oh2_objc_register_class_method("NSNumber",    "numberWithUnsignedLongLong:", (void *)&m5_nsnum_ull);
    oh2_objc_register_class_method("NSNumber",    "numberWithUnsignedInteger:",  (void *)&m5_nsnum_ull);
    oh2_objc_register_class_method("NSNumber",    "numberWithFloat:",            (void *)&m5_nsnum_float);
    oh2_objc_register_instance_method("NSNumber", "unsignedLongLongValue",       (void *)&m5_nsnum_ullVal);
    oh2_objc_register_instance_method("NSNumber", "unsignedIntegerValue",        (void *)&m5_nsnum_ullVal);
    oh2_objc_register_instance_method("NSNumber", "stringValue",                 (void *)&m5_nsnum_stringValue);

    oh2_objc_register_class_method("NSData",           "dataWithContentsOfURL:",          (void *)&m5_nsdata_dataWithURL);
    oh2_objc_register_instance_method("NSData",        "base64EncodedStringWithOptions:", (void *)&m5_nsdata_base64);
    oh2_objc_register_class_method("NSMutableData",    "data",                            (void *)&m2_nsdata_data);
    oh2_objc_register_instance_method("NSMutableData", "initWithLength:",                 (void *)&m5_nsmdata_initWithLen);
    oh2_objc_register_instance_method("NSMutableData", "length",                          (void *)&m5_nsmdata_length);
    oh2_objc_register_instance_method("NSMutableData", "bytes",                           (void *)&m5_nsmdata_bytes);
    oh2_objc_register_instance_method("NSMutableData", "mutableBytes",                    (void *)&m5_nsmdata_mutableBytes);
    oh2_objc_register_instance_method("NSMutableData", "appendBytes:length:",             (void *)&m5_nsmdata_appendBytes);
    oh2_objc_register_instance_method("NSMutableData", "appendData:",                     (void *)&m5_nsmdata_appendData);

    oh2_objc_register_instance_method("NSString", "rangeOfString:",                                 (void *)&m5_nsstr_rangeOfString);
    oh2_objc_register_instance_method("NSString", "substringFromIndex:",                            (void *)&m5_nsstr_substringFrom);
    oh2_objc_register_instance_method("NSString", "stringByReplacingOccurrencesOfString:withString:", (void *)&m5_nsstr_replaceOcc);
    oh2_objc_register_instance_method("NSString", "dataUsingEncoding:allowLossyConversion:",        (void *)&m5_nsstr_dataUsingEncoding);
    oh2_objc_register_instance_method("NSString", "initWithBytesNoCopy:length:encoding:freeWhenDone:", (void *)&m2_nsstring_initWithBytes);
    oh2_objc_register_instance_method("NSString", "initWithData:encoding:",                         (void *)&m5_nsstr_initWithDataEnc);
    oh2_objc_register_instance_method("NSString", "characterAtIndex:",                              (void *)&m5_nsstr_charAtIdx);
    oh2_objc_register_instance_method("NSString", "compare:",                                       (void *)&m5_nsstr_compare);
    oh2_objc_register_instance_method("NSString", "stringByDeletingPathExtension",                  (void *)&m5_nsstr_delPathExt);
    oh2_objc_register_instance_method("NSString", "componentsSeparatedByString:",                   (void *)&m5_nsstr_componentsSep);
    oh2_objc_register_instance_method("NSString", "stringByAddingPercentEncodingWithAllowedCharacters:", (void *)&m5_nsstr_percentEncode);
    oh2_objc_register_instance_method("NSString", "stringByRemovingPercentEncoding",                (void *)&m5_ns_ret_self);
    oh2_objc_register_instance_method("NSString", "writeToFile:atomically:encoding:error:",         (void *)&m5_nsstr_writeToFile);

    oh2_objc_register_instance_method("NSBundle", "localizedStringForKey:value:table:", (void *)&m5_nsbundle_localizedString);
    oh2_objc_register_instance_method("NSBundle", "bundleURL",                          (void *)&m5_nsbundle_bundleURL);
    oh2_objc_register_instance_method("NSBundle", "privateFrameworksPath",              (void *)&m5_nsbundle_privFw);

    oh2_objc_register_instance_method("NSFileManager", "attributesOfFileSystemForPath:error:", (void *)&m5_nsfm_fsAttrs);
    oh2_objc_register_instance_method("NSFileManager", "moveItemAtURL:toURL:error:",           (void *)&m5_nsfm_moveURL);

    oh2_objc_register_class_method("NSURL",    "fileURLWithPath:isDirectory:",     (void *)&m2_nsurl_fileURLWithPath);
    oh2_objc_register_instance_method("NSURL", "URL",                              (void *)&m5_ns_ret_self);
    oh2_objc_register_instance_method("NSURL", "getResourceValue:forKey:error:",   (void *)&m5_ns_ret_no);

    oh2_objc_register_instance_method("NSArray",        "initWithObjects:count:",    (void *)&m5_nsarr_initWithObjsCount);
    oh2_objc_register_instance_method("NSArray",        "indexOfObject:",            (void *)&m5_nsarr_indexOfObject);
    oh2_objc_register_instance_method("NSDictionary",   "objectForKeyedSubscript:",  (void *)&m5_nsdict_subscript);
    oh2_objc_register_instance_method("NSDictionary",   "allKeys",                   (void *)&m5_nsdict_allKeys);
    oh2_objc_register_class_method("NSDictionary",      "dictionaryWithObject:forKey:", (void *)&m5_nsdict_dictWithObjKey);
    oh2_objc_register_class_method("NSDictionary",      "dictionaryWithObjects:forKeys:count:", (void *)&m5_nsdict_dictWithObjsKeysCount);
    oh2_objc_register_instance_method("NSMutableDictionary", "setObject:forKeyedSubscript:", (void *)&m5_nsdict_setSubscript);

    oh2_objc_register_class_method("NSError",    "errorWithDomain:code:userInfo:", (void *)&m5_nserr_errorWithDomain);
    oh2_objc_register_instance_method("NSError", "code",                           (void *)&m5_nserr_code);
    oh2_objc_register_instance_method("NSError", "localizedDescription",           (void *)&m5_ns_ret_str_oh2);

    oh2_objc_register_instance_method("NSProcessInfo", "processName",              (void *)&m5_ns_ret_str_oh2);
    oh2_objc_register_instance_method("NSProcessInfo", "processID",                (void *)&m5_ns_ret_pid);
    oh2_objc_register_instance_method("NSProcessInfo", "isLowPowerModeEnabled",    (void *)&m5_ns_ret_no);
    oh2_objc_register_instance_method("NSProcessInfo", "thermalState",             (void *)&m5_ns_ret_no);

    oh2_objc_register_class_method("NSLocale",           "currentLocale",          (void *)&m5_ns_singleton);
    oh2_objc_register_class_method("NSLocale",           "preferredLanguages",     (void *)&m5_ns_prefLangs);
    oh2_objc_register_class_method("NSTimeZone",         "defaultTimeZone",        (void *)&m5_ns_singleton);
    oh2_objc_register_class_method("NSCalendar",         "autoupdatingCurrentCalendar", (void *)&m5_ns_singleton);
    oh2_objc_register_class_method("NSCharacterSet",     "URLPathAllowedCharacterSet", (void *)&m5_ns_singleton);
    oh2_objc_register_class_method("NSAssertionHandler", "currentHandler",         (void *)&m5_ns_singleton);
    oh2_objc_register_instance_method("NSAssertionHandler", "handleFailureInMethod:object:file:lineNumber:description:", (void *)&m5_ns_noop);
    oh2_objc_register_class_method("NSJSONSerialization", "JSONObjectWithData:options:error:", (void *)&m5_ns_ret_nil);
    oh2_objc_register_class_method("NSJSONSerialization", "dataWithJSONObject:options:error:", (void *)&m5_ns_ret_nil);
    oh2_objc_register_class_method("NSOperationQueue",   "mainQueue",              (void *)&m5_nsop_mainQueue);
    oh2_objc_register_class_method("NSOperationQueue",   "currentQueue",           (void *)&m5_nsop_mainQueue);
    oh2_objc_register_instance_method("NSOperationQueue","addOperationWithBlock:", (void *)&m5_nsop_addBlock);
    oh2_objc_register_instance_method("NSBundleResourceRequest", "initWithTags:",  (void *)&m5_ns_ret_self);
    oh2_objc_register_instance_method("NSBundleResourceRequest", "beginAccessingResourcesWithCompletionHandler:", (void *)&m5_nsbr_beginAccess);
    oh2_objc_register_instance_method("NSBundleResourceRequest", "endAccessingResources", (void *)&m5_ns_noop);
    oh2_objc_register_class_method("NSObject",    "instancesRespondToSelector:",   (void *)&m5_ns_ret_yes);
    oh2_objc_register_instance_method("NSObject", "retainCount",                   (void *)&m5_ns_ret_yes);
    oh2_objc_register_instance_method("NSObject", "valueForKey:",                  (void *)&m5_ns_ret_nil);
    oh2_objc_register_instance_method("NSObject", "addObserver:forKeyPath:options:context:", (void *)&m5_ns_noop);
    oh2_objc_register_instance_method("NSObject", "removeObserver:forKeyPath:",    (void *)&m5_ns_noop);
}

} /* extern "C" */
