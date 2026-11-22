/*
 * objc_runtime.cpp — M2 Objective-C dispatch, ARC/autoreleasepool,
 * @synchronized, Apple Blocks (_Block_copy/release), and GCD libdispatch stubs.
 */

#include "oh2_runtime.h"
#include "../loader/macho_loader.h"
#include "../libc_wrapper/libc_wrapper.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

extern "C" {
void oh2_objc_msgSend(void);
void oh2_objc_msgSendSuper2(void);
void oh2_objc_msgSend_stret(void);
}

/* ============================================================================
 * 1. Real @synchronized recursive per-object lock table
 * ============================================================================ */
namespace {

static std::mutex g_sync_table_mutex;
static std::unordered_map<const void *, pthread_mutex_t *> g_sync_object_locks;

static pthread_mutex_t *get_or_create_sync_lock(const void *obj) {
    std::lock_guard<std::mutex> lk(g_sync_table_mutex);
    auto it = g_sync_object_locks.find(obj);
    if (it != g_sync_object_locks.end()) return it->second;

    pthread_mutex_t *m = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    g_sync_object_locks[obj] = m;
    return m;
}

/* ============================================================================
 * 2. Thread-local NSAutoreleasePool stack & ARC retain/release
 * ============================================================================ */
struct AutoreleaseFrame {
    std::vector<void *> objects;
};

static thread_local std::vector<AutoreleaseFrame> tl_autorelease_stack;

/* ============================================================================
 * 3. Blocks (_Block_copy / _Block_release)
 * ============================================================================ */
enum {
    OH2_BLOCK_DEALLOCATING     = 0x0001,
    OH2_BLOCK_REFCOUNT_MASK    = 0xfffe,
    OH2_BLOCK_NEEDS_FREE       = (1 << 24),
    OH2_BLOCK_HAS_COPY_DISPOSE = (1 << 25),
    OH2_BLOCK_IS_GLOBAL        = (1 << 28),
};

struct DarwinBlockDescBasic {
    uintptr_t reserved;
    uintptr_t size;
};

struct DarwinBlockDescCopy {
    uintptr_t reserved;
    uintptr_t size;
    void (*copy_helper)(void *dst, const void *src);
    void (*dispose_helper)(const void *src);
};

struct DarwinBlockLayout {
    void    *isa;
    int32_t  flags;
    int32_t  reserved;
    void   (*invoke)(void *, ...);
    DarwinBlockDescBasic *descriptor;
};

/* ============================================================================
 * 4. Grand Central Dispatch (libdispatch) main & global queues + semaphores
 * ============================================================================ */
static uint8_t g_m2_main_queue_obj[64]   = {1};
static uint8_t g_m2_global_queue_obj[64] = {2};
static std::mutex g_main_q_mutex;
static std::deque<DarwinBlockLayout *> g_main_q_blocks;

struct PerformSelectorTask {
    void           *target;
    const char     *selector;
    void           *arg;
    bool            wait;
    bool            done;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
};
static std::deque<PerformSelectorTask *> g_perform_tasks;

struct DarwinDispatchSemaM2 {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    long            count;
};

static std::atomic<int> g_hit_metal_rhi_boundary{0};

} // namespace

extern "C" {

static void *oh2_objc_retain(void *obj) {
    if (!obj) return nullptr;
    if (oh2_objc_is_heap_object(obj)) {
        oh2_objc_add_refcount(obj, 1);
    }
    return obj;
}

static void oh2_objc_release(void *obj) {
    if (!obj) return;
    if (oh2_objc_is_heap_object(obj)) {
        oh2_objc_add_refcount(obj, -1);
    }
}

static void *oh2_objc_autorelease(void *obj) {
    if (!obj) return nullptr;
    if (!tl_autorelease_stack.empty()) {
        tl_autorelease_stack.back().objects.push_back(obj);
    }
    return obj;
}

static void *oh2_objc_autoreleasePoolPush(void) {
    tl_autorelease_stack.emplace_back();
    return (void *)(uintptr_t)tl_autorelease_stack.size();
}

static void oh2_objc_autoreleasePoolPop(void *pool_token) {
    if (tl_autorelease_stack.empty()) return;
    size_t target_depth = (size_t)(uintptr_t)pool_token;
    if (target_depth == 0 || target_depth > tl_autorelease_stack.size()) {
        target_depth = tl_autorelease_stack.size();
    }
    while (tl_autorelease_stack.size() >= target_depth && !tl_autorelease_stack.empty()) {
        AutoreleaseFrame frame = std::move(tl_autorelease_stack.back());
        tl_autorelease_stack.pop_back();
        for (void *obj : frame.objects) {
            oh2_objc_release(obj);
        }
    }
}

static int oh2_objc_sync_enter(void *obj) {
    if (!obj) return 0;
    return pthread_mutex_lock(get_or_create_sync_lock(obj));
}

static int oh2_objc_sync_exit(void *obj) {
    if (!obj) return 0;
    return pthread_mutex_unlock(get_or_create_sync_lock(obj));
}

static void *oh2_Block_copy(const void *arg) {
    if (!arg) return nullptr;
    const DarwinBlockLayout *src = (const DarwinBlockLayout *)arg;
    if (src->flags & OH2_BLOCK_IS_GLOBAL) {
        return const_cast<void *>(arg);
    }
    if (src->flags & OH2_BLOCK_NEEDS_FREE) {
        __atomic_fetch_add((int32_t *)&src->flags, 2, __ATOMIC_RELAXED);
        return const_cast<void *>(arg);
    }
    size_t sz = (src->descriptor && src->descriptor->size >= sizeof(DarwinBlockLayout) &&
                 src->descriptor->size < 65536)
                ? (size_t)src->descriptor->size
                : 128;
    DarwinBlockLayout *copy = (DarwinBlockLayout *)malloc(sz);
    if (!copy) return nullptr;
    memcpy(copy, src, sz);
    copy->flags = (src->flags & ~OH2_BLOCK_REFCOUNT_MASK) | OH2_BLOCK_NEEDS_FREE | 2;
    if ((src->flags & OH2_BLOCK_HAS_COPY_DISPOSE) && src->descriptor) {
        const DarwinBlockDescCopy *dc = (const DarwinBlockDescCopy *)src->descriptor;
        if (dc->copy_helper) {
            dc->copy_helper(copy, src);
        }
    }
    return copy;
}

static void oh2_Block_release(const void *arg) {
    if (!arg) return;
    DarwinBlockLayout *b = (DarwinBlockLayout *)const_cast<void *>(arg);
    if (b->flags & OH2_BLOCK_IS_GLOBAL) return;
    if (!(b->flags & OH2_BLOCK_NEEDS_FREE)) return;

    int32_t prev = __atomic_fetch_sub(&b->flags, 2, __ATOMIC_ACQ_REL);
    int32_t rc = prev & OH2_BLOCK_REFCOUNT_MASK;
    if (rc <= 2) {
        if ((b->flags & OH2_BLOCK_HAS_COPY_DISPOSE) && b->descriptor) {
            const DarwinBlockDescCopy *dc = (const DarwinBlockDescCopy *)b->descriptor;
            if (dc->dispose_helper) {
                dc->dispose_helper(b);
            }
        }
        free(b);
    }
}

static void oh2_Block_object_assign(void *dstAddr, const void *object, int flags) {
    (void)flags;
    if (!dstAddr) return;
    if ((flags & 0x07) == 7 && object) { /* BLOCK_FIELD_IS_BLOCK */
        *(void **)dstAddr = oh2_Block_copy(object);
    } else {
        *(const void **)dstAddr = object;
    }
}

static void oh2_Block_object_dispose(const void *object, int flags) {
    if ((flags & 0x07) == 7 && object) {
        oh2_Block_release(object);
    }
}

static void *gcd_async_worker(void *arg) {
    DarwinBlockLayout *b = (DarwinBlockLayout *)arg;
    if (b && b->invoke) {
        b->invoke(b);
    }
    oh2_Block_release(b);
    return nullptr;
}

static void *oh2_dispatch_get_global_queue(long identifier, unsigned long flags) {
    (void)identifier; (void)flags;
    return g_m2_global_queue_obj;
}

static void *oh2_dispatch_get_main_queue(void) {
    return g_m2_main_queue_obj;
}

static void *oh2_dispatch_queue_create(const char *label, const void *attr) {
    (void)label; (void)attr;
    return g_m2_global_queue_obj;
}

static uint64_t oh2_dispatch_time(uint64_t when, int64_t delta) {
    if (when == ~0ULL) return ~0ULL;
    uint64_t base = when;
    if (base == 0ULL) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        base = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    if (delta < 0) {
        uint64_t sub = (uint64_t)(-delta);
        return (base > sub) ? (base - sub) : 0ULL;
    }
    return base + (uint64_t)delta;
}

static void oh2_dispatch_async(void *queue, void *block) {
    if (!block) return;
    DarwinBlockLayout *b = (DarwinBlockLayout *)oh2_Block_copy(block);
    if (!b) return;

    void *main_q_sym = darwin_resolve_symbol(nullptr, "__dispatch_main_q");
    if (queue == g_m2_main_queue_obj || queue == main_q_sym) {
        std::lock_guard<std::mutex> lk(g_main_q_mutex);
        g_main_q_blocks.push_back(b);
        return;
    }

    pthread_t th;
    if (oh2_darwin_pthread_create(&th, nullptr, gcd_async_worker, b) == 0) {
        pthread_detach(th);
    } else {
        gcd_async_worker(b);
    }
}

static void oh2_dispatch_sync(void *queue, void *block) {
    (void)queue;
    if (!block) return;
    DarwinBlockLayout *b = (DarwinBlockLayout *)block;
    if (b->invoke) {
        b->invoke(b);
    }
}

static void oh2_dispatch_once_f(int64_t *predicate, void *context, void (*function)(void *)) {
    static pthread_mutex_t once_mtx = PTHREAD_MUTEX_INITIALIZER;
    if (!predicate || !function) return;
    if (__atomic_load_n(predicate, __ATOMIC_ACQUIRE) == ~0LL) return;
    pthread_mutex_lock(&once_mtx);
    if (__atomic_load_n(predicate, __ATOMIC_RELAXED) != ~0LL) {
        function(context);
        __atomic_store_n(predicate, ~0LL, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_mtx);
}

static void *oh2_dispatch_semaphore_create(long value) {
    DarwinDispatchSemaM2 *s = (DarwinDispatchSemaM2 *)calloc(1, sizeof(DarwinDispatchSemaM2));
    if (!s) return nullptr;
    pthread_mutex_init(&s->mtx, nullptr);
    pthread_cond_init(&s->cv, nullptr);
    s->count = value;
    return s;
}

static long oh2_dispatch_semaphore_signal(void *dsema) {
    if (!dsema) return 0;
    DarwinDispatchSemaM2 *s = (DarwinDispatchSemaM2 *)dsema;
    pthread_mutex_lock(&s->mtx);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    return 1;
}

static long oh2_dispatch_semaphore_wait(void *dsema, uint64_t timeout) {
    if (!dsema) return 0;
    DarwinDispatchSemaM2 *s = (DarwinDispatchSemaM2 *)dsema;
    pthread_mutex_lock(&s->mtx);
    if (timeout == 0ULL) {
        if (s->count > 0) {
            s->count--;
            pthread_mutex_unlock(&s->mtx);
            return 0;
        }
        pthread_mutex_unlock(&s->mtx);
        return 1; /* timed out */
    }
    if (timeout == ~0ULL) {
        while (s->count <= 0) {
            pthread_cond_wait(&s->cv, &s->mtx);
        }
        s->count--;
        pthread_mutex_unlock(&s->mtx);
        return 0;
    }

    struct timespec now_mono, now_rt, abs_rt;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    clock_gettime(CLOCK_REALTIME, &now_rt);
    uint64_t mono_ns = (uint64_t)now_mono.tv_sec * 1000000000ULL + (uint64_t)now_mono.tv_nsec;
    uint64_t rem_ns  = (timeout > mono_ns) ? (timeout - mono_ns) : 0ULL;
    uint64_t target_rt_ns = (uint64_t)now_rt.tv_sec * 1000000000ULL + (uint64_t)now_rt.tv_nsec + rem_ns;
    abs_rt.tv_sec  = (time_t)(target_rt_ns / 1000000000ULL);
    abs_rt.tv_nsec = (long)(target_rt_ns % 1000000000ULL);

    int rc = 0;
    while (s->count <= 0 && rc == 0) {
        rc = pthread_cond_timedwait(&s->cv, &s->mtx, &abs_rt);
    }
    if (s->count > 0) {
        s->count--;
        pthread_mutex_unlock(&s->mtx);
        return 0;
    }
    pthread_mutex_unlock(&s->mtx);
    return 1;
}

void oh2_dispatch_drain_main_queue(void) {
    for (;;) {
        DarwinBlockLayout *b = nullptr;
        PerformSelectorTask *pt = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_main_q_mutex);
            if (!g_main_q_blocks.empty()) {
                b = g_main_q_blocks.front();
                g_main_q_blocks.pop_front();
            } else if (!g_perform_tasks.empty()) {
                pt = g_perform_tasks.front();
                g_perform_tasks.pop_front();
            } else {
                break;
            }
        }
        if (b) {
            if (b->invoke) b->invoke(b);
            oh2_Block_release(b);
        } else if (pt) {
            typedef void (*msg1_t)(void *, const char *, void *);
            ((msg1_t)oh2_objc_msgSend)(pt->target, pt->selector, pt->arg);
            if (pt->wait) {
                pthread_mutex_lock(&pt->mtx);
                pt->done = true;
                pthread_cond_signal(&pt->cv);
                pthread_mutex_unlock(&pt->mtx);
            } else {
                delete pt;
            }
        }
    }
}

void oh2_perform_selector_on_main_thread(void *target, const char *selector, void *arg, int wait) {
    if (!target || !selector) return;
    if (getpid() == gettid()) {
        typedef void (*msg1_t)(void *, const char *, void *);
        ((msg1_t)oh2_objc_msgSend)(target, selector, arg);
        return;
    }
    PerformSelectorTask *pt = new PerformSelectorTask();
    pt->target   = target;
    pt->selector = oh2_objc_intern_selector(selector);
    pt->arg      = arg;
    pt->wait     = (wait != 0);
    pt->done     = false;
    pthread_mutex_init(&pt->mtx, nullptr);
    pthread_cond_init(&pt->cv, nullptr);

    if (pt->wait) {
        pthread_mutex_lock(&pt->mtx);
    }
    {
        std::lock_guard<std::mutex> lk(g_main_q_mutex);
        g_perform_tasks.push_back(pt);
    }
    if (pt->wait) {
        while (!pt->done) {
            pthread_cond_wait(&pt->cv, &pt->mtx);
        }
        pthread_mutex_unlock(&pt->mtx);
        pthread_cond_destroy(&pt->cv);
        pthread_mutex_destroy(&pt->mtx);
        delete pt;
    }
}

static void *oh2_m2_MTLCreateSystemDefaultDevice(void) {
    if (g_hit_metal_rhi_boundary.exchange(1) == 0) {
        oh2_log_info("[M2] Reached Metal RHI boundary (_MTLCreateSystemDefaultDevice -> deferred to M5)");
    }
    return nullptr;
}

int oh2_runtime_did_hit_metal_boundary(void) {
    return g_hit_metal_rhi_boundary.load();
}

void oh2_runtime_reset_metal_boundary(void) {
    g_hit_metal_rhi_boundary.store(0);
}

void oh2_m2_register_objc_runtime(void) {
    oh2_symbol_register_override("_objc_msgSend",                  (void *)&oh2_objc_msgSend);
    oh2_symbol_register_override("_objc_msgSendSuper2",            (void *)&oh2_objc_msgSendSuper2);
    oh2_symbol_register_override("_objc_msgSend_stret",            (void *)&oh2_objc_msgSend_stret);
    oh2_symbol_register_override("_objc_retain",                   (void *)&oh2_objc_retain);
    oh2_symbol_register_override("_objc_release",                  (void *)&oh2_objc_release);
    oh2_symbol_register_override("_objc_autorelease",              (void *)&oh2_objc_autorelease);
    oh2_symbol_register_override("_objc_autoreleasePoolPush",      (void *)&oh2_objc_autoreleasePoolPush);
    oh2_symbol_register_override("_objc_autoreleasePoolPop",       (void *)&oh2_objc_autoreleasePoolPop);
    oh2_symbol_register_override("_objc_sync_enter",               (void *)&oh2_objc_sync_enter);
    oh2_symbol_register_override("_objc_sync_exit",                (void *)&oh2_objc_sync_exit);

    oh2_symbol_register_override("__Block_copy",                   (void *)&oh2_Block_copy);
    oh2_symbol_register_override("__Block_release",                (void *)&oh2_Block_release);
    oh2_symbol_register_override("__Block_object_assign",          (void *)&oh2_Block_object_assign);
    oh2_symbol_register_override("__Block_object_dispose",         (void *)&oh2_Block_object_dispose);

    oh2_symbol_register_override("_dispatch_async",                (void *)&oh2_dispatch_async);
    oh2_symbol_register_override("_dispatch_sync",                 (void *)&oh2_dispatch_sync);
    oh2_symbol_register_override("_dispatch_get_global_queue",     (void *)&oh2_dispatch_get_global_queue);
    oh2_symbol_register_override("_dispatch_get_main_queue",       (void *)&oh2_dispatch_get_main_queue);
    oh2_symbol_register_override("_dispatch_queue_create",         (void *)&oh2_dispatch_queue_create);
    oh2_symbol_register_override("_dispatch_time",                 (void *)&oh2_dispatch_time);
    oh2_symbol_register_override("_dispatch_once_f",               (void *)&oh2_dispatch_once_f);
    oh2_symbol_register_override("_dispatch_semaphore_create",     (void *)&oh2_dispatch_semaphore_create);
    oh2_symbol_register_override("_dispatch_semaphore_signal",     (void *)&oh2_dispatch_semaphore_signal);
    oh2_symbol_register_override("_dispatch_semaphore_wait",       (void *)&oh2_dispatch_semaphore_wait);

    oh2_symbol_register_override("_MTLCreateSystemDefaultDevice",  (void *)&oh2_m2_MTLCreateSystemDefaultDevice);
}

} /* extern "C" */
