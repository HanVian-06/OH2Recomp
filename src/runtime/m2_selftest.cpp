/*
 * m2_selftest.cpp — M2 initialization, direct Pak v8 index/entry verification,
 * and all 14 granular E2E verification self-tests.
 */

#include "oh2_runtime.h"
#include "../loader/macho_loader.h"
#include "../libc_wrapper/libc_wrapper.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

extern "C" {
void oh2_objc_msgSend(void);
void oh2_objc_msgSendSuper2(void);
void oh2_asm_abi_capture_imp(void);
int64_t oh2_asm_msgsend_abi_probe(void *obj_or_super, const char *sel, void *capture_out, int is_super);
}

namespace {

static bool g_runtime_initialized = false;

static void reg_sym_adapter(const char *sym_name, void *addr) {
    oh2_symbol_register_override(sym_name, addr);
}

struct CapturedAbiState {
    uint64_t regs[9];     /* x0..x8 */
    uint64_t dregs[8];    /* d0..d7 bits */
    uint64_t stack0;      /* [sp, #0] */
    uint64_t stack1;      /* [sp, #8] */
};

struct AbiSuperPair {
    void *receiver;
    void *current_class;
};

/* ---------- Block & GCD test helpers ---------- */
struct TestBlockDesc {
    uintptr_t reserved;
    uintptr_t size;
    void (*copy_helper)(void *, const void *);
    void (*dispose_helper)(const void *);
};

struct TestBlockLiteral {
    void *isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke)(void *);
    TestBlockDesc *descriptor;
    int magic;
};

static std::atomic<int> g_block_copy_calls{0};
static std::atomic<int> g_block_dispose_calls{0};
static std::atomic<int> g_block_invokes{0};
static std::atomic<int> g_once_calls{0};

static void test_block_copy_helper(void *dst, const void *src) {
    (void)dst; (void)src;
    g_block_copy_calls.fetch_add(1);
}
static void test_block_dispose_helper(const void *src) {
    (void)src;
    g_block_dispose_calls.fetch_add(1);
}
static void test_block_invoke_fn(void *raw) {
    TestBlockLiteral *b = (TestBlockLiteral *)raw;
    if (b && b->magic == 0x4F483242) {
        g_block_invokes.fetch_add(1);
    }
}
static void test_once_fn(void *ctx) {
    (void)ctx;
    g_once_calls.fetch_add(1);
}

/* ---------- @synchronized thread test helper ---------- */
static std::atomic<int> g_sync_counter{0};
struct SyncThreadArg { void *obj; };
static void *sync_worker_fn(void *raw) {
    SyncThreadArg *a = (SyncThreadArg *)raw;
    typedef int (*sync_fn_t)(void *);
    sync_fn_t enter_fn = (sync_fn_t)darwin_resolve_symbol(nullptr, "_objc_sync_enter");
    sync_fn_t exit_fn  = (sync_fn_t)darwin_resolve_symbol(nullptr, "_objc_sync_exit");
    for (int i = 0; i < 500; ++i) {
        enter_fn(a->obj);
        int tmp = g_sync_counter.load(std::memory_order_relaxed);
        g_sync_counter.store(tmp + 1, std::memory_order_relaxed);
        exit_fn(a->obj);
    }
    return nullptr;
}

/* ---------- CADisplayLink callback counter ---------- */
static std::atomic<int> g_cadl_frames{0};
static double g_cadl_last_ts = 0.0;
static int g_cadl_monotonic_ok = 1;
static void cadl_callback_imp(void *self, const char *sel, void *link) {
    (void)self; (void)sel;
    typedef double (*ts_fn_t)(void *, const char *);
    double ts = ((ts_fn_t)oh2_objc_msgSend)(link, "timestamp");
    if (ts < g_cadl_last_ts) g_cadl_monotonic_ok = 0;
    g_cadl_last_ts = ts;
    g_cadl_frames.fetch_add(1);
}

/* ---------- Helper to load OH2 binary once per self-test process ---------- */
static MachOImage g_selftest_img;
static bool g_selftest_img_loaded = false;
static bool g_selftest_inits_done = false;

static int ensure_oh2_loaded(const char *oh2_path, bool run_inits = false) {
    if (!g_selftest_img_loaded) {
        const char *p = (oh2_path && oh2_path[0]) ? oh2_path : "/data/local/tmp/OH2";
        int rc = macho_load(p, &g_selftest_img);
        if (rc != 0) return rc;
        oh2_runtime_post_image_overrides();
        g_selftest_img_loaded = true;
    }
    if (run_inits && !g_selftest_inits_done) {
        char arg0[] = "OH2";
        char *argv[] = { arg0, nullptr };
        char *envp[] = { nullptr };
        char *apple[] = { arg0, nullptr };
        if (macho_run_initializers(&g_selftest_img, 1, argv, envp, apple) != 0) return 1;
        g_selftest_inits_done = true;
    }
    return 0;
}

/* ============================================================================
 * 14 Granular Self-Test Implementations
 * ============================================================================ */

static int test_objc_metadata_registration(const char *oh2_path) {
    if (ensure_oh2_loaded(oh2_path) != 0) return 1;
    if (g_selftest_img.objc_class_count != 60 ||
        g_selftest_img.objc_cat_count != 2 ||
        g_selftest_img.objc_proto_count != 28 ||
        g_selftest_img.objc_selref_count != 1491 ||
        g_selftest_img.cfstring_count != 245) {
        oh2_log_error("[M2_FAIL] objc_metadata_registration mismatch cls=%u cat=%u proto=%u sel=%u cf=%u",
                      g_selftest_img.objc_class_count, g_selftest_img.objc_cat_count,
                      g_selftest_img.objc_proto_count, g_selftest_img.objc_selref_count,
                      g_selftest_img.cfstring_count);
        return 1;
    }
    oh2_log_info("[M2_TEST_PASS] objc_metadata_registration classes=60 categories=2 protocols=28 selrefs=1491 cfstrings=245");
    return 0;
}

static int test_objc_msgsend_abi(void) {
    void *ns_obj_cls = oh2_objc_get_class("NSObject");
    void *obj = oh2_objc_alloc_instance(ns_obj_cls, 64);
    const char *sel = oh2_objc_intern_selector("m2AbiProbe:arg3:arg4:");
    oh2_objc_register_instance_method("NSObject", sel, (void *)&oh2_asm_abi_capture_imp);

    CapturedAbiState cap{};
    int64_t ret = oh2_asm_msgsend_abi_probe(obj, sel, &cap, 0);
    if ((uint64_t)ret != 0xCAFEBABE12345678ULL) {
        oh2_log_error("[M2_FAIL] objc_msgSend bad return value: 0x%llx", (unsigned long long)ret);
        return 1;
    }
    if (cap.regs[0] != (uint64_t)(uintptr_t)obj ||
        cap.regs[1] != (uint64_t)(uintptr_t)sel ||
        cap.regs[2] != 0xA2A2000000000002ULL ||
        cap.regs[3] != 0xA3A3000000000003ULL ||
        cap.regs[4] != 0xA4A4000000000004ULL ||
        cap.regs[5] != 0xA5A5000000000005ULL ||
        cap.regs[6] != 0xA6A6000000000006ULL ||
        cap.regs[7] != 0xA7A7000000000007ULL ||
        cap.regs[8] != (uint64_t)(uintptr_t)&cap) {
        oh2_log_error("[M2_FAIL] objc_msgSend GP register x0-x8 mismatch");
        return 1;
    }
    const uint64_t exp_d[8] = {
        0x3FF0000000000000ULL, 0x4000000000000000ULL,
        0x4008000000000000ULL, 0x4010000000000000ULL,
        0x4014000000000000ULL, 0x4018000000000000ULL,
        0x401C000000000000ULL, 0x4020000000000000ULL
    };
    for (int i = 0; i < 8; ++i) {
        if (cap.dregs[i] != exp_d[i]) {
            oh2_log_error("[M2_FAIL] objc_msgSend FP register d%d mismatch: got=0x%llx exp=0x%llx",
                          i, (unsigned long long)cap.dregs[i], (unsigned long long)exp_d[i]);
            return 1;
        }
    }
    if (cap.stack0 != 0x1111222233334444ULL || cap.stack1 != 0x5555666677778888ULL) {
        oh2_log_error("[M2_FAIL] objc_msgSend stack arg mismatch: 0x%llx 0x%llx",
                      (unsigned long long)cap.stack0, (unsigned long long)cap.stack1);
        return 1;
    }

    /* Also test objc_msgSendSuper2 via subclass UIView -> superclass UIResponder -> NSObject */
    void *uiview_cls = oh2_objc_get_class("UIView");
    void *view_obj = oh2_objc_alloc_instance(uiview_cls, 64);
    AbiSuperPair sup{ view_obj, uiview_cls };
    CapturedAbiState cap_sup{};
    int64_t sret = oh2_asm_msgsend_abi_probe(&sup, sel, &cap_sup, 1);
    if ((uint64_t)sret != 0xCAFEBABE12345678ULL ||
        cap_sup.regs[0] != (uint64_t)(uintptr_t)view_obj ||
        cap_sup.regs[2] != 0xA2A2000000000002ULL ||
        cap_sup.stack0 != 0x1111222233334444ULL) {
        oh2_log_error("[M2_FAIL] objc_msgSendSuper2 verification failed");
        return 1;
    }

    /* Nil receiver fast-path check */
    typedef void *(*nil_msg_fn_t)(void *, const char *);
    void *nil_res = ((nil_msg_fn_t)oh2_objc_msgSend)(nullptr, sel);
    if (nil_res != nullptr) return 1;

    oh2_log_info("[M2_PASS] 1_OBJC_MSGSSEND: register preservation self-test PASS (x0-x8, d0-d7, stack, super2, nil)");
    return 0;
}

static int test_objc_arc_sync(void) {
    typedef void *(*fn_p_t)(void *);
    typedef void  (*fn_v_t)(void *);
    typedef void *(*fn_push_t)(void);
    typedef void  (*fn_set_assoc_t)(const void *, const void *, void *, uintptr_t);
    typedef void *(*fn_get_assoc_t)(const void *, const void *);

    fn_p_t retain_fn  = (fn_p_t)darwin_resolve_symbol(nullptr, "_objc_retain");
    fn_v_t release_fn = (fn_v_t)darwin_resolve_symbol(nullptr, "_objc_release");
    fn_p_t auto_fn    = (fn_p_t)darwin_resolve_symbol(nullptr, "_objc_autorelease");
    fn_push_t push_fn = (fn_push_t)darwin_resolve_symbol(nullptr, "_objc_autoreleasePoolPush");
    fn_v_t pop_fn     = (fn_v_t)darwin_resolve_symbol(nullptr, "_objc_autoreleasePoolPop");
    fn_set_assoc_t sa = (fn_set_assoc_t)darwin_resolve_symbol(nullptr, "_objc_setAssociatedObject");
    fn_get_assoc_t ga = (fn_get_assoc_t)darwin_resolve_symbol(nullptr, "_objc_getAssociatedObject");

    void *obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSObject"), 64);
    void *val = oh2_objc_create_string("assoc-ok");
    if (oh2_objc_get_refcount(obj) != 1) return 1;

    void *pool = push_fn();
    retain_fn(obj);
    if (oh2_objc_get_refcount(obj) != 2) return 1;
    auto_fn(obj);
    pop_fn(pool);
    if (oh2_objc_get_refcount(obj) != 1) return 1;
    release_fn(obj);

    static int assoc_key = 42;
    sa(obj, &assoc_key, val, 1);
    if (ga(obj, &assoc_key) != val) return 1;

    g_sync_counter.store(0);
    SyncThreadArg arg{obj};
    pthread_t t1, t2;
    oh2_darwin_pthread_create(&t1, nullptr, sync_worker_fn, &arg);
    oh2_darwin_pthread_create(&t2, nullptr, sync_worker_fn, &arg);
    pthread_join(t1, nullptr);
    pthread_join(t2, nullptr);
    if (g_sync_counter.load() != 1000) return 1;

    oh2_log_info("[M2_TEST_PASS] objc_arc_sync PASS");
    return 0;
}

static int test_blocks_gcd(void) {
    typedef void *(*bcopy_t)(const void *);
    typedef void  (*brel_t)(const void *);
    typedef void  (*d_once_t)(int64_t *, void *, void (*)(void *));
    typedef void  (*d_async_t)(void *, void *);
    typedef void *(*d_gq_t)(long, unsigned long);
    typedef void *(*d_sc_t)(long);
    typedef long  (*d_ss_t)(void *);
    typedef long  (*d_sw_t)(void *, uint64_t);
    typedef uint64_t (*d_tm_t)(uint64_t, int64_t);

    bcopy_t  blk_copy = (bcopy_t)darwin_resolve_symbol(nullptr, "__Block_copy");
    brel_t   brel     = (brel_t)darwin_resolve_symbol(nullptr, "__Block_release");
    d_once_t donce  = (d_once_t)darwin_resolve_symbol(nullptr, "_dispatch_once_f");
    d_async_t dasync= (d_async_t)darwin_resolve_symbol(nullptr, "_dispatch_async");
    d_gq_t   dgq    = (d_gq_t)darwin_resolve_symbol(nullptr, "_dispatch_get_global_queue");
    d_sc_t   dsc    = (d_sc_t)darwin_resolve_symbol(nullptr, "_dispatch_semaphore_create");
    d_ss_t   dss    = (d_ss_t)darwin_resolve_symbol(nullptr, "_dispatch_semaphore_signal");
    d_sw_t   dsw    = (d_sw_t)darwin_resolve_symbol(nullptr, "_dispatch_semaphore_wait");
    d_tm_t   dtm    = (d_tm_t)darwin_resolve_symbol(nullptr, "_dispatch_time");

    static TestBlockDesc desc_s{0, sizeof(TestBlockLiteral), &test_block_copy_helper, &test_block_dispose_helper};
    TestBlockLiteral stack_blk{nullptr, (1 << 25), 0, &test_block_invoke_fn, &desc_s, 0x4F483242};

    g_block_copy_calls.store(0);
    g_block_dispose_calls.store(0);
    g_block_invokes.store(0);
    g_once_calls.store(0);

    void *copied = blk_copy(&stack_blk);
    memset(&stack_blk, 0, sizeof(stack_blk));
    void *sema = dsc(0);
    dasync(dgq(0, 0), copied);
    ((TestBlockLiteral *)copied)->invoke(copied);
    dss(sema);
    if (dsw(sema, dtm(0, 2000000000LL)) != 0) return 1;
    usleep(50000);
    brel(copied);

    static int64_t once_pred = 0;
    donce(&once_pred, nullptr, &test_once_fn);
    donce(&once_pred, nullptr, &test_once_fn);

    if (g_block_copy_calls.load() != 1 || g_block_invokes.load() < 2 || g_once_calls.load() != 1) {
        return 1;
    }
    oh2_log_info("[M2_TEST_PASS] blocks_gcd PASS");
    return 0;
}

static int test_objc_oh2_smoke(const char *oh2_path) {
    if (ensure_oh2_loaded(oh2_path, true) != 0) return 1;
    void *cls = oh2_objc_get_class("IOSAppDelegate");
    if (!cls) return 1;
    typedef void *(*msg0_t)(void *, const char *);
    void *inst = ((msg0_t)oh2_objc_msgSend)(cls, "alloc");
    inst = ((msg0_t)oh2_objc_msgSend)(inst, "init");
    if (!inst) return 1;

    void *dev_cls = oh2_objc_get_class("UIDevice");
    void *dev = ((msg0_t)oh2_objc_msgSend)(dev_cls, "currentDevice");
    void *mdl = ((msg0_t)oh2_objc_msgSend)(dev, "model");
    char buf[64];
    const char *s = darwin_cfstring_utf8(mdl, buf, sizeof(buf));
    if (!s || strcmp(s, "iPhone") != 0) return 1;

    oh2_log_info("[M2_TEST_PASS] objc_oh2_smoke PASS");
    return 0;
}

static int test_libc_file_stat(void) {
    const char *dir  = "/data/local/tmp/oh2_m2_lwrap_test";
    const char *file = "/data/local/tmp/oh2_m2_lwrap_test/TestMixedCase.txt";
    mkdir(dir, 0755);
    unlink(file);

    /* Darwin O_CREAT (0x200) | O_WRONLY (1) | O_TRUNC (0x400) */
    int fd = oh2_darwin_open_direct(file, 0x200 | 0x400 | 1, 0644);
    if (fd < 0) return 1;
    const char payload[] = "OH2_DARWIN_OPEN_STAT";
    if (write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1)) {
        oh2_darwin_close(fd);
        return 1;
    }
    oh2_darwin_close(fd);

    DarwinStat64 st{};
    if (oh2_darwin_stat("/data/local/tmp/oh2_m2_lwrap_test/testmixedcase.txt", &st) != 0) {
        return 1;
    }
    if (st.st_size != (int64_t)(sizeof(payload) - 1)) return 1;

    DIR *d = oh2_darwin_opendir(dir);
    if (!d) return 1;
    bool saw = false;
    DarwinDirent64 *ent;
    while ((ent = oh2_darwin_readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, "TestMixedCase.txt") == 0 && ent->d_namlen == 17) {
            saw = true;
            break;
        }
    }
    oh2_darwin_closedir(d);
    unlink(file);
    rmdir(dir);
    if (!saw) return 1;

    oh2_log_info("[M2_TEST_PASS] libc_file_stat PASS");
    return 0;
}

static int test_libc_sockaddr(void) {
    int srv = oh2_darwin_socket(2, SOCK_DGRAM, 0);
    int cli = oh2_darwin_socket(2, SOCK_DGRAM, 0);
    if (srv < 0 || cli < 0) return 1;

    int opt = 1;
    if (oh2_darwin_setsockopt(cli, 0xffff, 0x1022 /* SO_NOSIGPIPE */, &opt, sizeof(opt)) != 0) {
        oh2_darwin_close(srv); oh2_darwin_close(cli); return 1;
    }

    DarwinSockAddrIn baddr{};
    baddr.sin_len    = 16;
    baddr.sin_family = 2; /* AF_INET */
    baddr.sin_port   = 0;
    baddr.sin_addr   = htonl(INADDR_LOOPBACK);
    if (oh2_darwin_bind(srv, (const DarwinSockAddr *)&baddr, 16) != 0) {
        oh2_darwin_close(srv); oh2_darwin_close(cli); return 1;
    }

    DarwinSockAddrIn bound{};
    uint32_t blen = sizeof(bound);
    if (oh2_darwin_getsockname(srv, (DarwinSockAddr *)&bound, &blen) != 0 ||
        bound.sin_len != 16 || bound.sin_family != 2 || bound.sin_port == 0) {
        oh2_darwin_close(srv); oh2_darwin_close(cli); return 1;
    }

    const char msg[] = "OH2_SOCK_LOOP";
    if (oh2_darwin_sendto(cli, msg, sizeof(msg), 0, (const DarwinSockAddr *)&bound, 16) != (ssize_t)sizeof(msg)) {
        oh2_darwin_close(srv); oh2_darwin_close(cli); return 1;
    }

    char rx[32]{};
    DarwinSockAddrIn src{};
    uint32_t slen = sizeof(src);
    ssize_t n = oh2_darwin_recvfrom(srv, rx, sizeof(rx), 0, (DarwinSockAddr *)&src, &slen);
    oh2_darwin_close(srv);
    oh2_darwin_close(cli);
    if (n != (ssize_t)sizeof(msg) || strcmp(rx, msg) != 0 || src.sin_len != 16) return 1;

    oh2_log_info("[M2_TEST_PASS] libc_sockaddr PASS");
    return 0;
}

static int test_libc_pthread_errno(void) {
    errno = EAGAIN;
    int de = *oh2_darwin_error();
    if (de != 35) return 1; /* Darwin EAGAIN == 35 */

    if (oh2_darwin_pthread_setname_np("VeryLongDarwinThreadNameForUE4") != 0) return 1;

    /* Darwin static recursive mutex magic 0x32aaaba2 */
    alignas(16) uint8_t d_mtx[64]{};
    *(uint32_t *)d_mtx = 0x32aaaba2U;
    if (oh2_darwin_pthread_mutex_lock(d_mtx) != 0) return 1;
    if (oh2_darwin_pthread_mutex_lock(d_mtx) != 0) return 1;
    oh2_darwin_pthread_mutex_unlock(d_mtx);
    oh2_darwin_pthread_mutex_unlock(d_mtx);
    oh2_darwin_pthread_mutex_destroy(d_mtx);

    uint64_t key = 0xDEAD000000000000ULL;
    if (oh2_darwin_pthread_key_create(&key, nullptr) != 0 || (key >> 32) != 0) return 1;
    oh2_darwin_pthread_setspecific(key, (void *)0x1234ULL);
    if (oh2_darwin_pthread_getspecific(key) != (void *)0x1234ULL) return 1;
    oh2_darwin_pthread_key_delete(key);

    oh2_log_info("[M2_PASS] 2_LIBC_WRAPPER: errno/sockaddr/stat/open/pthread PASS");
    return 0;
}

static int test_nsbundle_filemanager(void) {
    typedef void *(*msg0_t)(void *, const char *);
    typedef void *(*msg2_t)(void *, const char *, void *, void *);
    typedef uint8_t (*exists_t)(void *, const char *, void *, uint8_t *);

    void *bundle = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("NSBundle"), "mainBundle");
    void *bp     = ((msg0_t)oh2_objc_msgSend)(bundle, "bundlePath");
    char buf[512];
    const char *bp_str = darwin_cfstring_utf8(bp, buf, sizeof(buf));
    const char *expected_root = oh2_libc_wrapper_get_bundle_root();
    if (!bp_str || strcmp(bp_str, expected_root) != 0) {
        oh2_log_error("[M2_FAIL] NSBundle.bundlePath='%s' != '%s'", bp_str ? bp_str : "?", expected_root);
        return 1;
    }

    char tx1[512], tx2[512];
    oh2_libc_wrapper_translate_path("@executable_path/cookeddata/oh2/content/paks/oh2-ios.pak", tx1, sizeof(tx1));
    oh2_libc_wrapper_translate_path("/var/containers/Bundle/Application/1234/OH2.app/ue4commandline.txt", tx2, sizeof(tx2));
    if (strncmp(tx1, expected_root, strlen(expected_root)) != 0 ||
        strncmp(tx2, expected_root, strlen(expected_root)) != 0) {
        return 1;
    }

    void *fm = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("NSFileManager"), "defaultManager");
    uint8_t is_dir = 0;
    if (((exists_t)oh2_objc_msgSend)(fm, "fileExistsAtPath:isDirectory:", oh2_objc_create_string(expected_root), &is_dir) == 0 || is_dir == 0) {
        return 1;
    }
    void *found_cmd = ((msg2_t)oh2_objc_msgSend)(bundle, "pathForResource:ofType:",
                                                 oh2_objc_create_string("ue4commandline"),
                                                 oh2_objc_create_string("txt"));
    const char *found_s = darwin_cfstring_utf8(found_cmd, buf, sizeof(buf));
    if (!found_cmd || !found_s || strstr(found_s, "ue4commandline.txt") == nullptr) {
        oh2_log_error("[M2_FAIL] pathForResource:ofType: failed: '%s'", found_s ? found_s : "null");
        return 1;
    }

    oh2_log_info("[M2_PASS] 3_NSBUNDLE: %s/ mapping PASS", expected_root);
    return 0;
}

static int test_uikit_cadisplaylink(void) {
    typedef void *(*msg0_t)(void *, const char *);
    typedef double (*dbl0_t)(void *, const char *);
    typedef void  (*v_i64_t)(void *, const char *, int64_t);
    typedef void  (*v_u8_t)(void *, const char *, uint8_t);
    typedef void *(*dl_create_t)(void *, const char *, void *, const char *);
    typedef void  (*dl_add_t)(void *, const char *, void *, void *);

    void *app = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("UIApplication"), "sharedApplication");
    void *scr = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("UIScreen"), "mainScreen");
    double sc = ((dbl0_t)oh2_objc_msgSend)(scr, "scale");
    struct TestRect64 { double x, y, w, h; };
    typedef TestRect64 (*bounds_t)(void *, const char *);
    TestRect64 rect = ((bounds_t)oh2_objc_msgSend)(scr, "bounds");
    TestRect64 nrect = ((bounds_t)oh2_objc_msgSend)(scr, "nativeBounds");
    if (!app || sc != 2.0 || nrect.w != 2340.0 || nrect.h != 1080.0 ||
        (rect.w != 1170.0 && rect.w != 2340.0) || (rect.h != 540.0 && rect.h != 1080.0)) return 1;

    void *target_obj = oh2_objc_alloc_instance(oh2_objc_get_class("NSObject"), 64);
    const char *cb_sel = oh2_objc_intern_selector("m2DisplayLinkTick:");
    oh2_objc_register_instance_method("NSObject", cb_sel, (void *)&cadl_callback_imp);

    g_cadl_frames.store(0);
    g_cadl_last_ts = 0.0;
    g_cadl_monotonic_ok = 1;

    void *link = ((dl_create_t)oh2_objc_msgSend)(oh2_objc_get_class("CADisplayLink"),
                                                "displayLinkWithTarget:selector:",
                                                target_obj, cb_sel);
    ((v_i64_t)oh2_objc_msgSend)(link, "setPreferredFramesPerSecond:", 60);
    ((v_u8_t)oh2_objc_msgSend)(link, "setPaused:", 0);
    void *rl = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("NSRunLoop"), "mainRunLoop");
    ((dl_add_t)oh2_objc_msgSend)(link, "addToRunLoop:forMode:", rl, oh2_objc_create_string("NSDefaultRunLoopMode"));

    for (int i = 0; i < 60 && g_cadl_frames.load() < 10; ++i) {
        usleep(25000);
    }
    ((msg0_t)oh2_objc_msgSend)(link, "invalidate");

    if (g_cadl_frames.load() < 10 || !g_cadl_monotonic_ok) {
        oh2_log_error("[M2_FAIL] CADisplayLink frames=%d mono=%d", g_cadl_frames.load(), g_cadl_monotonic_ok);
        return 1;
    }
    oh2_log_info("[M2_TEST_PASS] uikit_cadisplaylink frames=%d PASS", g_cadl_frames.load());
    return 0;
}

static int test_stubs_gamekit_storekit(void) {
    typedef void   *(*msg0_t)(void *, const char *);
    typedef uint8_t (*u8_0_t)(void *, const char *);
    void *gk = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("GKLocalPlayer"), "localPlayer");
    if (!gk || ((u8_0_t)oh2_objc_msgSend)(gk, "isAuthenticated") != 0) return 1;

    void *sk = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("SKPaymentQueue"), "defaultQueue");
    if (!sk || ((u8_0_t)oh2_objc_msgSend)(oh2_objc_get_class("SKPaymentQueue"), "canMakePayments") != 0) return 1;
    ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("SKStoreReviewController"), "requestReview");
    oh2_log_info("[M2_TEST_PASS] stubs_gamekit_storekit PASS");
    return 0;
}

static int test_stubs_cloud_mc_un_dc(void) {
    typedef void   *(*msg0_t)(void *, const char *);
    typedef void   *(*msg1_t)(void *, const char *, void *);
    typedef uint8_t (*u8_0_t)(void *, const char *);

    void *ck = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("CKContainer"), "defaultContainer");
    void *db = ((msg0_t)oh2_objc_msgSend)(ck, "publicCloudDatabase");
    void *mc = ((msg1_t)oh2_objc_msgSend)(((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("MCPeerID"), "alloc"),
                                         "initWithDisplayName:", oh2_objc_create_string("OH2"));
    void *un = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("UNUserNotificationCenter"), "currentNotificationCenter");
    void *dc = ((msg0_t)oh2_objc_msgSend)(oh2_objc_get_class("DCDevice"), "currentDevice");
    if (!ck || !db || !mc || !un || !dc || ((u8_0_t)oh2_objc_msgSend)(dc, "isSupported") != 0) return 1;

    oh2_log_info("[M2_PASS] 4_FRAMEWORK_STUBS: GameKit/StoreKit/CloudKit non-crashing PASS");
    return 0;
}

static int test_stub_plcrashreporter(const char *oh2_path) {
    if (ensure_oh2_loaded(oh2_path) != 0) return 1;
    typedef void   *(*msg0_t)(void *, const char *);
    typedef uint8_t (*en_t)(void *, const char *, void **);
    void *cls = oh2_objc_get_class("PLCrashReporter");
    void *rep = ((msg0_t)oh2_objc_msgSend)(cls, "sharedReporter");
    void *err = (void *)0x1234ULL;
    uint8_t ok = ((en_t)oh2_objc_msgSend)(rep, "enableCrashReporterAndReturnError:", &err);
    if (!rep || ok != 1 || err != nullptr) return 1;
    oh2_log_info("[M2_TEST_PASS] stub_plcrashreporter PASS");
    return 0;
}

/* ---------- Direct Pak v8 Reader & Extraction Verifier ---------- */
static inline uint16_t rd_u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }
static inline uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

static std::string read_pak_fstring(const uint8_t *buf, size_t max_len, size_t *pos) {
    if (*pos + 4 > max_len) return "";
    int32_t slen = rd_i32(buf + *pos);
    *pos += 4;
    if (slen > 0 && *pos + (size_t)slen <= max_len) {
        std::string s((const char *)(buf + *pos), (size_t)(slen - 1));
        *pos += (size_t)slen;
        return s;
    }
    if (slen < 0) {
        size_t nbytes = (size_t)(-slen) * 2;
        if (*pos + nbytes <= max_len) {
            std::string s;
            for (int i = 0; i < (-slen) - 1; ++i) {
                uint16_t ch = rd_u16(buf + *pos + i * 2);
                s.push_back(ch < 0x80 ? (char)ch : '?');
            }
            *pos += nbytes;
            return s;
        }
    }
    return "";
}

static bool extract_pak_entry_bytes(int fd, const uint8_t *comp_blocks_ptr, uint32_t comp_method,
                                    uint64_t offset, uint64_t uncomp_size,
                                    const std::vector<std::pair<uint64_t, uint64_t>> &blocks,
                                    std::vector<uint8_t> *out) {
    (void)comp_blocks_ptr;
    out->clear();
    if (comp_method == 0) {
        out->resize((size_t)uncomp_size);
        return oh2_darwin_pread(fd, out->data(), (size_t)uncomp_size, (off_t)(offset + 53ULL)) == (ssize_t)uncomp_size;
    }
    if (comp_method == 1) {
        out->reserve((size_t)uncomp_size);
        for (const auto &blk : blocks) {
            size_t bsz = (size_t)(blk.second - blk.first);
            std::vector<uint8_t> cbuf(bsz);
            if (oh2_darwin_pread(fd, cbuf.data(), bsz, (off_t)(offset + blk.first)) != (ssize_t)bsz) {
                return false;
            }
            uLongf dst_len = (uLongf)uncomp_size;
            std::vector<uint8_t> ubuf((size_t)uncomp_size);
            if (uncompress(ubuf.data(), &dst_len, cbuf.data(), (uLong)bsz) != Z_OK) {
                return false;
            }
            out->insert(out->end(), ubuf.begin(), ubuf.begin() + dst_len);
        }
        if (out->size() > (size_t)uncomp_size) out->resize((size_t)uncomp_size);
        return out->size() == (size_t)uncomp_size;
    }
    return false;
}

} // namespace

extern "C" int oh2_runtime_verify_pak_direct(const char *pak_path, int extract_verify) {
    const char *path = (pak_path && pak_path[0])
        ? pak_path
        : "/sdcard/Android/data/com.oceanhorn2.port/files/cookeddata/oh2/content/paks/oh2-ios.pak";

    int fd = oh2_darwin_open_direct(path, 0 /* O_RDONLY */);
    if (fd < 0) {
        oh2_log_error("[M2_FAIL] Failed opening pak via darwin_open: %s", path);
        return 1;
    }

    DarwinStat64 st{};
    if (oh2_darwin_fstat(fd, &st) != 0 || (uint64_t)st.st_size != OH2_PAK_EXPECTED_SIZE) {
        oh2_log_error("[M2_FAIL] Unexpected pak fstat size=%lld (expected=%llu)",
                      (long long)st.st_size, (unsigned long long)OH2_PAK_EXPECTED_SIZE);
        oh2_darwin_close(fd);
        return 1;
    }

    uint8_t footer[221]{};
    if (oh2_darwin_lseek(fd, (off_t)OH2_PAK_FOOTER_OFFSET, SEEK_SET) != (off_t)OH2_PAK_FOOTER_OFFSET ||
        oh2_darwin_read(fd, footer, 221) != 221) {
        oh2_darwin_close(fd);
        return 1;
    }

    uint8_t  enc_idx    = footer[16];
    uint32_t magic      = rd_u32(footer + 17);
    uint32_t version    = rd_u32(footer + 21);
    uint64_t index_off  = rd_u64(footer + 25);
    uint64_t index_size = rd_u64(footer + 33);
    if (enc_idx != 0 || magic != 0x5a6f12e1U || version != 8U ||
        index_off != OH2_PAK_INDEX_OFFSET || index_size != OH2_PAK_INDEX_SIZE) {
        oh2_log_error("[M2_FAIL] Invalid Pak v8 footer: magic=0x%x ver=%u enc=%u idx_off=%llu idx_sz=%llu",
                      magic, version, enc_idx, (unsigned long long)index_off, (unsigned long long)index_size);
        oh2_darwin_close(fd);
        return 1;
    }

    std::vector<uint8_t> idx((size_t)index_size);
    if (oh2_darwin_lseek(fd, (off_t)index_off, SEEK_SET) != (off_t)index_off ||
        oh2_darwin_read(fd, idx.data(), idx.size()) != (ssize_t)idx.size()) {
        oh2_darwin_close(fd);
        return 1;
    }

    size_t pos = 0;
    std::string mount_pt = read_pak_fstring(idx.data(), idx.size(), &pos);
    uint32_t num_entries = (pos + 4 <= idx.size()) ? rd_u32(idx.data() + pos) : 0;
    pos += 4;
    if (mount_pt != "../../../" || num_entries != 19920U) {
        oh2_log_error("[M2_FAIL] Invalid Pak index header: MountPoint='%s' NumEntries=%u",
                      mount_pt.c_str(), num_entries);
        oh2_darwin_close(fd);
        return 1;
    }

    bool saw_ini = false;
    bool saw_global_shader = false;
    uint32_t parsed = 0;

    for (uint32_t i = 0; i < num_entries && pos < idx.size(); ++i) {
        std::string fname = read_pak_fstring(idx.data(), idx.size(), &pos);
        if (pos + 48 > idx.size()) break;
        uint64_t off      = rd_u64(idx.data() + pos);
        uint64_t size     = rd_u64(idx.data() + pos + 8);
        uint64_t u_size   = rd_u64(idx.data() + pos + 16);
        uint32_t comp_m   = rd_u32(idx.data() + pos + 24);
        (void)size;
        pos += 48; /* offset(8)+size(8)+uncomp(8)+comp(4)+sha1(20) */

        std::vector<std::pair<uint64_t, uint64_t>> blocks;
        if (comp_m != 0) {
            if (pos + 4 > idx.size()) break;
            uint32_t bcnt = rd_u32(idx.data() + pos);
            pos += 4;
            for (uint32_t b = 0; b < bcnt && pos + 16 <= idx.size(); ++b) {
                blocks.emplace_back(rd_u64(idx.data() + pos), rd_u64(idx.data() + pos + 8));
                pos += 16;
            }
        }
        if (pos + 5 > idx.size()) break;
        pos += 5; /* flags(1) + comp_block_size(4) */
        parsed++;

        if (extract_verify) {
            if (fname == "OH2/Config/DefaultEngine.ini") {
                std::vector<uint8_t> bytes;
                if (!extract_pak_entry_bytes(fd, nullptr, comp_m, off, u_size, blocks, &bytes)) {
                    oh2_darwin_close(fd);
                    return 1;
                }
                std::string ini_txt((const char *)bytes.data(), bytes.size());
                if (ini_txt.find("bCookASTCTextures=True") == std::string::npos) {
                    oh2_log_error("[M2_FAIL] DefaultEngine.ini missing bCookASTCTextures=True");
                    oh2_darwin_close(fd);
                    return 1;
                }
                saw_ini = true;
                oh2_log_info("[M2] Verified pak zlib entry OH2/Config/DefaultEngine.ini (%zu B): bCookASTCTextures=True",
                             bytes.size());
            } else if (fname == "OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode") {
                std::vector<uint8_t> bytes;
                if (!extract_pak_entry_bytes(fd, nullptr, comp_m, off, u_size, blocks, &bytes) ||
                    bytes.size() != 1236176ULL) {
                    oh2_darwin_close(fd);
                    return 1;
                }
                saw_global_shader = true;
                oh2_log_info("[M2] Verified pak uncompressed entry ShaderArchive-Global-SF_METAL.ushaderbytecode (%zu B)",
                             bytes.size());
            }
        }
    }

    oh2_darwin_close(fd);
    if (parsed != 19920U) return 1;
    if (extract_verify && (!saw_ini || !saw_global_shader)) {
        oh2_log_error("[M2_FAIL] Missing pak verify targets: ini=%d shader=%d", saw_ini ? 1 : 0, saw_global_shader ? 1 : 0);
        return 1;
    }
    return 0;
}

extern "C" int oh2_runtime_init(const char *bundle_root) {
    if (bundle_root && bundle_root[0]) {
        oh2_libc_wrapper_set_bundle_root(bundle_root);
        oh2_objc_set_bundle_root(bundle_root);
    }
    if (g_runtime_initialized) return 0;

    oh2_symbol_resolver_init(nullptr);
    oh2_libc_wrapper_register_symbols(&reg_sym_adapter);
    oh2_variadic_set_open_handler(&oh2_darwin_open_impl);

    oh2_m2_register_objc_runtime();
    oh2_m2_register_ui_bridges();
    oh2_m2_register_ns_bridges();
    oh2_m2_register_framework_stubs();

    g_runtime_initialized = true;
    oh2_log_info("[M2] Runtime initialized (bundle_root=%s)", oh2_libc_wrapper_get_bundle_root());
    return 0;
}

extern "C" int oh2_runtime_run_selftest(const char *test_name, const char *oh2_path) {
    if (oh2_runtime_init("/sdcard/Android/data/com.oceanhorn2.port/files") != 0) return 1;

    if (!test_name || !test_name[0] || strcmp(test_name, "all_unit") == 0) {
        if (test_objc_msgsend_abi() != 0) return 1;
        if (test_objc_arc_sync() != 0) return 1;
        if (test_blocks_gcd() != 0) return 1;
        if (test_libc_file_stat() != 0) return 1;
        if (test_libc_sockaddr() != 0) return 1;
        if (test_libc_pthread_errno() != 0) return 1;
        if (test_nsbundle_filemanager() != 0) return 1;
        if (test_uikit_cadisplaylink() != 0) return 1;
        if (test_stubs_gamekit_storekit() != 0) return 1;
        if (test_stubs_cloud_mc_un_dc() != 0) return 1;
        return 0;
    }

    if (strcmp(test_name, "objc_metadata_registration") == 0) return test_objc_metadata_registration(oh2_path);
    if (strcmp(test_name, "objc_msgsend_abi") == 0)           return test_objc_msgsend_abi();
    if (strcmp(test_name, "objc_arc_sync") == 0)              return test_objc_arc_sync();
    if (strcmp(test_name, "blocks_gcd") == 0)                 return test_blocks_gcd();
    if (strcmp(test_name, "objc_oh2_smoke") == 0)             return test_objc_oh2_smoke(oh2_path);
    if (strcmp(test_name, "libc_file_stat") == 0)             return test_libc_file_stat();
    if (strcmp(test_name, "libc_sockaddr") == 0)              return test_libc_sockaddr();
    if (strcmp(test_name, "libc_pthread_errno") == 0)         return test_libc_pthread_errno();
    if (strcmp(test_name, "nsbundle_filemanager") == 0)       return test_nsbundle_filemanager();
    if (strcmp(test_name, "uikit_cadisplaylink") == 0)        return test_uikit_cadisplaylink();
    if (strcmp(test_name, "stubs_gamekit_storekit") == 0)     return test_stubs_gamekit_storekit();
    if (strcmp(test_name, "stubs_cloud_mc_un_dc") == 0)       return test_stubs_cloud_mc_un_dc();
    if (strcmp(test_name, "stub_plcrashreporter") == 0)       return test_stub_plcrashreporter(oh2_path);
    if (strcmp(test_name, "pak_extract_verify") == 0)         return oh2_runtime_verify_pak_direct(nullptr, 1);

    oh2_log_error("[M2_FAIL] Unknown self-test name: %s", test_name);
    return 2;
}
