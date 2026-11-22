/*
 * pthread_wrapper.c — Darwin pthread ABI -> Bionic pthread wrappers
 * plus oh2_libc_wrapper_register_symbols() symbol table registration.
 */

#include "libc_wrapper.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DARWIN_MUTEX_MAGIC_BASE  0x32aaaba0U
#define DARWIN_MUTEX_RECURSIVE   0x32aaaba2U
#define DARWIN_COND_MAGIC_BASE   0x3cb0b1b0U
#define DARWIN_RWLOCK_MAGIC_BASE 0x2da8b3b0U
#define OH2_MIN_WORKER_STACK     (2U * 1024U * 1024U)

static pthread_mutex_t g_darwin_init_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *sanitize_mutex(void *m) {
    if (!m) return NULL;
    uint32_t sig = __atomic_load_n((uint32_t *)m, __ATOMIC_ACQUIRE);
    if (__builtin_expect((sig & 0xfffffff0U) == DARWIN_MUTEX_MAGIC_BASE, 0)) {
        pthread_mutex_lock(&g_darwin_init_lock);
        sig = __atomic_load_n((uint32_t *)m, __ATOMIC_RELAXED);
        if ((sig & 0xfffffff0U) == DARWIN_MUTEX_MAGIC_BASE) {
            memset(m, 0, 64);
            if (sig == DARWIN_MUTEX_RECURSIVE) {
                pthread_mutexattr_t attr;
                pthread_mutexattr_init(&attr);
                pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
                pthread_mutex_init((pthread_mutex_t *)m, &attr);
                pthread_mutexattr_destroy(&attr);
            } else {
                pthread_mutex_init((pthread_mutex_t *)m, NULL);
            }
        }
        pthread_mutex_unlock(&g_darwin_init_lock);
    }
    return (pthread_mutex_t *)m;
}

static pthread_cond_t *sanitize_cond(void *c) {
    if (!c) return NULL;
    uint32_t sig = __atomic_load_n((uint32_t *)c, __ATOMIC_ACQUIRE);
    if (__builtin_expect((sig & 0xfffffff0U) == DARWIN_COND_MAGIC_BASE, 0)) {
        pthread_mutex_lock(&g_darwin_init_lock);
        sig = __atomic_load_n((uint32_t *)c, __ATOMIC_RELAXED);
        if ((sig & 0xfffffff0U) == DARWIN_COND_MAGIC_BASE) {
            memset(c, 0, 48);
            pthread_cond_init((pthread_cond_t *)c, NULL);
        }
        pthread_mutex_unlock(&g_darwin_init_lock);
    }
    return (pthread_cond_t *)c;
}

static pthread_rwlock_t *sanitize_rwlock(void *rwl) {
    if (!rwl) return NULL;
    uint32_t sig = __atomic_load_n((uint32_t *)rwl, __ATOMIC_ACQUIRE);
    if (__builtin_expect((sig & 0xfffffff0U) == DARWIN_RWLOCK_MAGIC_BASE, 0)) {
        pthread_mutex_lock(&g_darwin_init_lock);
        sig = __atomic_load_n((uint32_t *)rwl, __ATOMIC_RELAXED);
        if ((sig & 0xfffffff0U) == DARWIN_RWLOCK_MAGIC_BASE) {
            memset(rwl, 0, sizeof(pthread_rwlock_t));
            pthread_rwlock_init((pthread_rwlock_t *)rwl, NULL);
        }
        pthread_mutex_unlock(&g_darwin_init_lock);
    }
    return (pthread_rwlock_t *)rwl;
}

int oh2_darwin_pthread_mutex_init(void *m, const void *attr) {
    if (!m) return EINVAL;
    memset(m, 0, 64);
    return pthread_mutex_init((pthread_mutex_t *)m, (const pthread_mutexattr_t *)attr);
}

int oh2_darwin_pthread_mutex_lock(void *m) {
    return m ? pthread_mutex_lock(sanitize_mutex(m)) : EINVAL;
}

int oh2_darwin_pthread_mutex_trylock(void *m) {
    if (!m) return EINVAL;
    int rc = pthread_mutex_trylock(sanitize_mutex(m));
    return (rc == 0) ? 0 : oh2_linux_to_darwin_errno(rc);
}

int oh2_darwin_pthread_mutex_unlock(void *m) {
    return m ? pthread_mutex_unlock(sanitize_mutex(m)) : EINVAL;
}

int oh2_darwin_pthread_mutex_destroy(void *m) {
    if (!m) return 0;
    uint32_t sig = __atomic_load_n((uint32_t *)m, __ATOMIC_ACQUIRE);
    if ((sig & 0xfffffff0U) == DARWIN_MUTEX_MAGIC_BASE) {
        memset(m, 0, 64);
        return 0;
    }
    return pthread_mutex_destroy((pthread_mutex_t *)m);
}

int oh2_darwin_pthread_mutexattr_init(void *attr) {
    return pthread_mutexattr_init((pthread_mutexattr_t *)attr);
}

int oh2_darwin_pthread_mutexattr_destroy(void *attr) {
    return pthread_mutexattr_destroy((pthread_mutexattr_t *)attr);
}

int oh2_darwin_pthread_mutexattr_settype(void *attr, int darwin_type) {
    int bionic_type = PTHREAD_MUTEX_NORMAL;
    if (darwin_type == 2) bionic_type = PTHREAD_MUTEX_RECURSIVE;
    else if (darwin_type == 1) bionic_type = PTHREAD_MUTEX_ERRORCHECK;
    return pthread_mutexattr_settype((pthread_mutexattr_t *)attr, bionic_type);
}

int oh2_darwin_pthread_mutexattr_setprotocol(void *attr, int protocol) {
    (void)attr;
    (void)protocol;
    return 0;
}

int oh2_darwin_pthread_cond_init(void *c, const void *attr) {
    if (!c) return EINVAL;
    memset(c, 0, 48);
    return pthread_cond_init((pthread_cond_t *)c, (const pthread_condattr_t *)attr);
}

int oh2_darwin_pthread_cond_wait(void *c, void *m) {
    if (!c || !m) return EINVAL;
    return pthread_cond_wait(sanitize_cond(c), sanitize_mutex(m));
}

int oh2_darwin_pthread_cond_timedwait(void *c, void *m, const struct timespec *abstime) {
    if (!c || !m) return EINVAL;
    int rc = pthread_cond_timedwait(sanitize_cond(c), sanitize_mutex(m), abstime);
    return (rc == 0) ? 0 : oh2_linux_to_darwin_errno(rc);
}

int oh2_darwin_pthread_cond_signal(void *c) {
    return c ? pthread_cond_signal(sanitize_cond(c)) : EINVAL;
}

int oh2_darwin_pthread_cond_broadcast(void *c) {
    return c ? pthread_cond_broadcast(sanitize_cond(c)) : EINVAL;
}

int oh2_darwin_pthread_cond_destroy(void *c) {
    if (!c) return 0;
    uint32_t sig = __atomic_load_n((uint32_t *)c, __ATOMIC_ACQUIRE);
    if ((sig & 0xfffffff0U) == DARWIN_COND_MAGIC_BASE) {
        memset(c, 0, 48);
        return 0;
    }
    return pthread_cond_destroy((pthread_cond_t *)c);
}

int oh2_darwin_pthread_rwlock_init(void *rwl, const void *attr) {
    if (!rwl) return EINVAL;
    memset(rwl, 0, sizeof(pthread_rwlock_t));
    return pthread_rwlock_init((pthread_rwlock_t *)rwl, (const pthread_rwlockattr_t *)attr);
}

int oh2_darwin_pthread_rwlock_rdlock(void *rwl) {
    return rwl ? pthread_rwlock_rdlock(sanitize_rwlock(rwl)) : EINVAL;
}

int oh2_darwin_pthread_rwlock_wrlock(void *rwl) {
    return rwl ? pthread_rwlock_wrlock(sanitize_rwlock(rwl)) : EINVAL;
}

int oh2_darwin_pthread_rwlock_unlock(void *rwl) {
    return rwl ? pthread_rwlock_unlock(sanitize_rwlock(rwl)) : EINVAL;
}

int oh2_darwin_pthread_rwlock_destroy(void *rwl) {
    if (!rwl) return 0;
    uint32_t sig = __atomic_load_n((uint32_t *)rwl, __ATOMIC_ACQUIRE);
    if ((sig & 0xfffffff0U) == DARWIN_RWLOCK_MAGIC_BASE) {
        memset(rwl, 0, sizeof(pthread_rwlock_t));
        return 0;
    }
    return pthread_rwlock_destroy((pthread_rwlock_t *)rwl);
}

int oh2_darwin_pthread_key_create(uint64_t *key_out, void (*destructor)(void *)) {
    if (!key_out) return EINVAL;
    pthread_key_t k = 0;
    int rc = pthread_key_create(&k, destructor);
    *key_out = (rc == 0) ? (uint64_t)(uint32_t)k : 0ULL;
    return rc;
}

int oh2_darwin_pthread_key_delete(uint64_t key) {
    return pthread_key_delete((pthread_key_t)(uint32_t)key);
}

void *oh2_darwin_pthread_getspecific(uint64_t key) {
    return pthread_getspecific((pthread_key_t)(uint32_t)key);
}

int oh2_darwin_pthread_setspecific(uint64_t key, const void *val) {
    return pthread_setspecific((pthread_key_t)(uint32_t)key, val);
}

int oh2_darwin_pthread_attr_setstacksize(void *attr, size_t stacksize) {
    if (!attr) return EINVAL;
    size_t safe_sz = (stacksize < OH2_MIN_WORKER_STACK) ? OH2_MIN_WORKER_STACK : stacksize;
    return pthread_attr_setstacksize((pthread_attr_t *)attr, safe_sz);
}

int oh2_darwin_pthread_create(void *thread_out, const void *attr, void *(*start)(void *), void *arg) {
    if (!thread_out || !start) return EINVAL;

    pthread_attr_t local_attr;
    int using_local = 0;
    const pthread_attr_t *use_attr = (const pthread_attr_t *)attr;

    if (!use_attr) {
        pthread_attr_init(&local_attr);
        pthread_attr_setstacksize(&local_attr, OH2_MIN_WORKER_STACK);
        use_attr = &local_attr;
        using_local = 1;
    } else {
        size_t cur_stack = 0;
        pthread_attr_getstacksize(use_attr, &cur_stack);
        if (cur_stack < OH2_MIN_WORKER_STACK) {
            int detach_state = PTHREAD_CREATE_JOINABLE;
            pthread_attr_getdetachstate(use_attr, &detach_state);
            pthread_attr_init(&local_attr);
            pthread_attr_setdetachstate(&local_attr, detach_state);
            pthread_attr_setstacksize(&local_attr, OH2_MIN_WORKER_STACK);
            use_attr = &local_attr;
            using_local = 1;
        }
    }

    int rc = pthread_create((pthread_t *)thread_out, use_attr, start, arg);
    if (using_local) {
        pthread_attr_destroy(&local_attr);
    }
    return rc;
}

int oh2_darwin_pthread_setname_np(const char *name) {
    char safe[16];
    if (!name || !name[0]) {
        snprintf(safe, sizeof(safe), "OH2Thread");
    } else {
        /* Linux prctl/pthread_setname_np strictly limits thread names to 15 chars + NUL */
        snprintf(safe, sizeof(safe), "%.15s", name);
    }
    return pthread_setname_np(pthread_self(), safe);
}

int oh2_darwin_pthread_cancel(uintptr_t th) {
    (void)th;
    return 0;
}

uint32_t oh2_darwin_pthread_mach_thread_np(uintptr_t th) {
    (void)th;
    return (uint32_t)gettid();
}

uintptr_t oh2_darwin_pthread_from_mach_thread_np(uint32_t port) {
    (void)port;
    return (uintptr_t)pthread_self();
}

static void oh2_darwin_std_mutex_lock(void *m) {
    oh2_darwin_pthread_mutex_lock(m);
}

static void oh2_darwin_std_mutex_unlock(void *m) {
    oh2_darwin_pthread_mutex_unlock(m);
}

void oh2_libc_wrapper_register_symbols(void (*reg_fn)(const char *sym_name, void *addr)) {
    if (!reg_fn) return;

    /* Errno */
    reg_fn("___error", (void *)&oh2_darwin_error);

    /* File / Path / I/O / Memory */
    reg_fn("_fopen",    (void *)&oh2_darwin_fopen);
    reg_fn("_fread",    (void *)&oh2_darwin_fread);
    reg_fn("_fseek",    (void *)&oh2_darwin_fseek);
    reg_fn("_fclose",   (void *)&oh2_darwin_fclose);
    reg_fn("_access",   (void *)&oh2_darwin_access);
    reg_fn("_lseek",    (void *)&oh2_darwin_lseek);
    reg_fn("_read",     (void *)&oh2_darwin_read);
    reg_fn("_pread",    (void *)&oh2_darwin_pread);
    reg_fn("_close",    (void *)&oh2_darwin_close);
    reg_fn("_unlink",   (void *)&oh2_darwin_unlink);
    reg_fn("_rename",   (void *)&oh2_darwin_rename);
    reg_fn("_rmdir",    (void *)&oh2_darwin_rmdir);
    reg_fn("_chmod",    (void *)&oh2_darwin_chmod);
    reg_fn("_utime",    (void *)&oh2_darwin_utime);
    reg_fn("_malloc",   (void *)&oh2_darwin_malloc);
    reg_fn("_mmap",     (void *)&oh2_darwin_mmap);
    reg_fn("_stat",     (void *)&oh2_darwin_stat);
    reg_fn("_lstat",    (void *)&oh2_darwin_lstat);
    reg_fn("_fstat",    (void *)&oh2_darwin_fstat);
    reg_fn("_opendir",  (void *)&oh2_darwin_opendir);
    reg_fn("_readdir",  (void *)&oh2_darwin_readdir);
    reg_fn("_closedir", (void *)&oh2_darwin_closedir);

    /* Sockets & DNS */
    reg_fn("_socket",      (void *)&oh2_darwin_socket);
    reg_fn("_bind",        (void *)&oh2_darwin_bind);
    reg_fn("_connect",     (void *)&oh2_darwin_connect);
    reg_fn("_accept",      (void *)&oh2_darwin_accept);
    reg_fn("_sendto",      (void *)&oh2_darwin_sendto);
    reg_fn("_recvfrom",    (void *)&oh2_darwin_recvfrom);
    reg_fn("_getsockname", (void *)&oh2_darwin_getsockname);
    reg_fn("_getpeername", (void *)&oh2_darwin_getpeername);
    reg_fn("_getsockopt",  (void *)&oh2_darwin_getsockopt);
    reg_fn("_setsockopt",  (void *)&oh2_darwin_setsockopt);
    reg_fn("_getaddrinfo", (void *)&oh2_darwin_getaddrinfo);
    reg_fn("_freeaddrinfo",(void *)&oh2_darwin_freeaddrinfo);
    reg_fn("_getnameinfo", (void *)&oh2_darwin_getnameinfo);
    reg_fn("_inet_pton",   (void *)&oh2_darwin_inet_pton);

    /* Pthread & std::__1::mutex */
    reg_fn("__ZNSt3__15mutex4lockEv",          (void *)&oh2_darwin_std_mutex_lock);
    reg_fn("__ZNSt3__15mutex6unlockEv",        (void *)&oh2_darwin_std_mutex_unlock);
    reg_fn("_pthread_mutex_init",              (void *)&oh2_darwin_pthread_mutex_init);
    reg_fn("_pthread_mutex_lock",              (void *)&oh2_darwin_pthread_mutex_lock);
    reg_fn("_pthread_mutex_trylock",           (void *)&oh2_darwin_pthread_mutex_trylock);
    reg_fn("_pthread_mutex_unlock",            (void *)&oh2_darwin_pthread_mutex_unlock);
    reg_fn("_pthread_mutex_destroy",           (void *)&oh2_darwin_pthread_mutex_destroy);
    reg_fn("_pthread_mutexattr_init",          (void *)&oh2_darwin_pthread_mutexattr_init);
    reg_fn("_pthread_mutexattr_destroy",       (void *)&oh2_darwin_pthread_mutexattr_destroy);
    reg_fn("_pthread_mutexattr_settype",       (void *)&oh2_darwin_pthread_mutexattr_settype);
    reg_fn("_pthread_mutexattr_setprotocol",   (void *)&oh2_darwin_pthread_mutexattr_setprotocol);
    reg_fn("_pthread_cond_init",               (void *)&oh2_darwin_pthread_cond_init);
    reg_fn("_pthread_cond_wait",               (void *)&oh2_darwin_pthread_cond_wait);
    reg_fn("_pthread_cond_timedwait",          (void *)&oh2_darwin_pthread_cond_timedwait);
    reg_fn("_pthread_cond_signal",             (void *)&oh2_darwin_pthread_cond_signal);
    reg_fn("_pthread_cond_broadcast",          (void *)&oh2_darwin_pthread_cond_broadcast);
    reg_fn("_pthread_cond_destroy",            (void *)&oh2_darwin_pthread_cond_destroy);
    reg_fn("_pthread_rwlock_init",             (void *)&oh2_darwin_pthread_rwlock_init);
    reg_fn("_pthread_rwlock_rdlock",           (void *)&oh2_darwin_pthread_rwlock_rdlock);
    reg_fn("_pthread_rwlock_wrlock",           (void *)&oh2_darwin_pthread_rwlock_wrlock);
    reg_fn("_pthread_rwlock_unlock",           (void *)&oh2_darwin_pthread_rwlock_unlock);
    reg_fn("_pthread_rwlock_destroy",          (void *)&oh2_darwin_pthread_rwlock_destroy);
    reg_fn("_pthread_key_create",              (void *)&oh2_darwin_pthread_key_create);
    reg_fn("_pthread_key_delete",              (void *)&oh2_darwin_pthread_key_delete);
    reg_fn("_pthread_getspecific",             (void *)&oh2_darwin_pthread_getspecific);
    reg_fn("_pthread_setspecific",             (void *)&oh2_darwin_pthread_setspecific);
    reg_fn("_pthread_create",                  (void *)&oh2_darwin_pthread_create);
    reg_fn("_pthread_attr_setstacksize",       (void *)&oh2_darwin_pthread_attr_setstacksize);
    reg_fn("_pthread_setname_np",              (void *)&oh2_darwin_pthread_setname_np);
    reg_fn("_pthread_cancel",                  (void *)&oh2_darwin_pthread_cancel);
    reg_fn("_pthread_mach_thread_np",          (void *)&oh2_darwin_pthread_mach_thread_np);
    reg_fn("_pthread_from_mach_thread_np",     (void *)&oh2_darwin_pthread_from_mach_thread_np);
}
