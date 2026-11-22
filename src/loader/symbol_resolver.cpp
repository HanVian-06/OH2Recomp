#include "macho_loader.h"

#include <android/log.h>
#include <ctype.h>
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* External assembly & C variadic entry points (variadic_trampolines.S / variadic_wrappers.c) */
extern "C" {
void darwin_printf(void);
void darwin_fprintf(void);
void darwin_sprintf(void);
void darwin_snprintf(void);
void darwin_sprintf_chk(void);
void darwin_snprintf_chk(void);
void darwin_sscanf(void);
void darwin_NSLog(void);
void darwin_open(void);
void darwin_fcntl(void);
void darwin_ioctl(void);
void darwin_syscall(void);
int  darwin_vfprintf(FILE *stream, const char *fmt, char *darwin_ap);
int  darwin_vsnprintf(char *s, size_t n, const char *fmt, char *darwin_ap);
int  darwin_vsnprintf_objc(char *out, size_t out_sz, const char *fmt, char *darwin_sp);

void darwin_objc_msgSend(void);
void darwin_objc_msgSendSuper2(void);
void darwin_objc_stringWithFormat(void);
void darwin_objc_appendFormat(void);

/* Unwind & Itanium C++ ABI symbols from -static-libstdc++ (libc++abi.a + libunwind.a) */
void _Unwind_Resume(void *exception_object);
int  __gxx_personality_v0(int version, int actions, uint64_t exception_class,
                          void *exception_object, void *context);
int  __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle);
void *__dynamic_cast(const void *sub, const void *src, const void *dst, ptrdiff_t src2dst_offset);
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
}

namespace std {
inline namespace __ndk1 {
size_t __next_prime(size_t n);
}
}

/* ============================================================================
 * 1. Darwin Objective-C & CFString ABI Layouts (64-bit ARM64 Mach-O)
 * ============================================================================ */

struct DarwinMethodEntry {
    const char *name;
    const char *types;
    void       *imp;
};

struct DarwinMethodList {
    uint32_t entsizeAndFlags;
    uint32_t count;
    DarwinMethodEntry entries[1];
};

struct DarwinClassRO {
    uint32_t          flags;
    uint32_t          instanceStart;
    uint32_t          instanceSize;
    uint32_t          reserved;
    const uint8_t    *ivarLayout;
    const char       *name;
    DarwinMethodList *baseMethodList;
    void             *baseProtocols;
    void             *ivars;
    const uint8_t    *weakIvarLayout;
    void             *baseProperties;
};

/* Exact 40-byte Mach-O 64-bit objc_class structure */
struct DarwinObjCClass {
    DarwinObjCClass *isa;
    DarwinObjCClass *superclass;
    void            *cache_buckets;
    uint64_t         cache_mask_occupied;
    uint64_t         data_ptr; /* points to DarwinClassRO (low 3 bits flags) */
};

static_assert(sizeof(DarwinObjCClass) == 40, "DarwinObjCClass must be 40 bytes");
static_assert(sizeof(DarwinClassRO) == 72, "DarwinClassRO must be 72 bytes");

struct DarwinCategory {
    const char       *name;
    DarwinObjCClass  *cls;
    DarwinMethodList *instanceMethods;
    DarwinMethodList *classMethods;
    void             *protocols;
    void             *instanceProperties;
};

struct DarwinObjCSuper {
    void            *receiver;
    DarwinObjCClass *current_class;
};

/* Exact 32-byte Mach-O 64-bit __builtin___CFStringMakeConstantString structure */
struct DarwinCFString {
    void       *isa;
    uint32_t    flags;
    uint32_t    dynamic_tag;
    const char *cstr;
    uint64_t    length;
};

static_assert(sizeof(DarwinCFString) == 32, "DarwinCFString must be 32 bytes");

struct CGRect64 {
    double x;
    double y;
    double width;
    double height;
};

/*
 * 128-byte negative prefix header allocated immediately BEFORE every heap ObjC instance.
 * Guarantees 0 collision with OH2 subclass ivars (e.g. FIOSView, IOSAppDelegate).
 */
#define OH2_OBJC_HEAP_MAGIC 0x4F48324FU /* 'OH2O' */

struct alignas(16) DarwinObjCPrefix {
    uint32_t    magic;
    int32_t     refcount;
    uint32_t    alloc_size;
    int32_t     tag;
    CGRect64    frame;
    void       *delegate;
    void       *window;
    void       *root_vc;
    void       *layer;
    void       *target;
    const char *selector_name;
    void       *user_info;
    std::string *cpp_str;
    std::vector<void *> *cpp_array;
    std::unordered_map<std::string, void *> *cpp_dict;
    int64_t     num_i64;
    double      num_f64;
};

static_assert(sizeof(DarwinObjCPrefix) % 16 == 0, "DarwinObjCPrefix must be 16-byte aligned");

#include "undef_805_table.inc"

/* ============================================================================
 * 2. Group-A (14) & Group-B (98) Data Globals Storage
 * ============================================================================ */

extern "C" {

/* Group-A (14 direct struct/array/Class-ISA objects) */
DarwinObjCClass ___CFConstantStringClassReference;
DarwinObjCClass __NSConcreteGlobalBlock;
DarwinObjCClass __NSConcreteStackBlock;
uint64_t __objc_empty_cache[2] = {0, 0};
uint64_t __dispatch_main_q[8]  = {1, 0, 0, 0, 0, 0, 0, 0};
uint8_t  _NDR_record[16]       = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
uint64_t __FE_DFL_ENV[2]       = {0, 0};
uint8_t  _in6addr_any[16]      = {0};
uint8_t  _in6addr_loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
char    *_tzname[2]            = {(char *)"UTC", (char *)"UTC"};
uint64_t _kCFTypeArrayCallBacks[6] = {0, 0, 0, 0, 0, 0};

struct DarwinCMTime {
    int64_t  value;
    int32_t  timescale;
    uint32_t flags;
    int64_t  epoch;
};
DarwinCMTime _kCMTimeZero             = {0, 1, 1, 0};
DarwinCMTime _kCMTimePositiveInfinity = {0, 0, 5, 0};

/* Darwin _RuneLocale (4,224 bytes): __runetype[256] at +0x3c, __maplower at +0x43c, __mapupper at +0x83c */
struct DarwinRuneLocale {
    char     __magic[8];
    char     __encoding[32];
    void    *__sgetrune;
    void    *__sputrune;
    int32_t  __invalid_rune;
    uint32_t __runetype[256];
    int32_t  __maplower[256];
    int32_t  __mapupper[256];
    uint8_t  __extra[1088];
};
static_assert(offsetof(DarwinRuneLocale, __runetype) == 0x3c, "__runetype must be at +0x3c");
static_assert(offsetof(DarwinRuneLocale, __maplower) == 0x43c, "__maplower must be at +0x43c");
static_assert(offsetof(DarwinRuneLocale, __mapupper) == 0x83c, "__mapupper must be at +0x83c");
DarwinRuneLocale __DefaultRuneLocale;

/* Group-B (18 non-string scalar/pointer variables) */
uint64_t ___stack_chk_guard                = 0x595e9fbd94fda766ULL;
FILE    *___stdinp                         = NULL;
FILE    *___stdoutp                        = NULL;
FILE    *___stderrp                        = NULL;
uint32_t _mach_task_self_                  = 0x103U;
uint64_t _vm_page_size                     = 4096ULL;
uint64_t _vm_page_mask                     = 4095ULL;
void    *___NSDictionary0__                = NULL;
void    *_kCFAllocatorDefault              = NULL;
void    *_kCFAllocatorSystemDefault        = NULL;
void    *kCFAllocatorNull                  = (void *)0x1;
int64_t  _kCIFormatARGB8                   = 266;
uint64_t _UIAccessibilityTraitNone         = 0;
uint64_t _UIAccessibilityTraitButton       = 1;
uint64_t _UIAccessibilityTraitLink         = 2;
uint64_t _UIAccessibilityTraitImage        = 4;
uint64_t _UIAccessibilityTraitNotEnabled   = 8;
uint64_t _UIAccessibilityTraitAdjustable   = 16;
uint64_t _CKQueryOperationMaximumResults   = 400;

/* Group-B (80 CFStringRef / NSString* pointer variables) */
#define DEFINE_GROUP_B_CFSTRING(sym, lit) \
    static DarwinCFString sym##_storage = { &___CFConstantStringClassReference, 0x07c8U, 0U, lit, sizeof(lit) - 1 }; \
    DarwinCFString *sym = &sym##_storage;

OH2_FOR_EACH_GROUP_B_CFSTRING(DEFINE_GROUP_B_CFSTRING)
#undef DEFINE_GROUP_B_CFSTRING

/* 140 External ObjC Classes + 140 External Metaclasses */
#define DEFINE_EXT_OBJC_CLASS(cls, sup) \
    DarwinClassRO   _OBJC_RO_META_$_##cls; \
    DarwinClassRO   _OBJC_RO_CLASS_$_##cls; \
    DarwinObjCClass _OBJC_METACLASS_$_##cls; \
    DarwinObjCClass _OBJC_CLASS_$_##cls;

OH2_FOR_EACH_EXT_OBJC_CLASS(DEFINE_EXT_OBJC_CLASS)
#undef DEFINE_EXT_OBJC_CLASS

static DarwinClassRO g_cfconst_str_meta_ro;
static DarwinClassRO g_cfconst_str_cls_ro;
static DarwinObjCClass g_cfconst_str_meta;

static DarwinClassRO g_global_block_ro;
static DarwinClassRO g_stack_block_ro;

} /* extern "C" */

/* ============================================================================
 * 3. Objective-C Runtime Registry & Selector Interning
 * ============================================================================ */

namespace {

static std::mutex g_objc_mutex;
static std::unordered_set<std::string> g_sel_pool;
static std::unordered_map<std::string, DarwinObjCClass *> g_class_by_name;
static std::unordered_map<const DarwinObjCClass *, std::unordered_map<const char *, void *>> g_method_table;
static std::unordered_map<const DarwinObjCClass *, std::unordered_map<std::string, void *>> g_method_table_str;
static std::unordered_set<uint64_t> g_logged_missing_sels;
static std::unordered_map<uint64_t, std::unordered_map<const void *, void *>> g_assoc_objects;

static const char *intern_selector(const char *name) {
    if (!name) return "";
    auto it = g_sel_pool.insert(name).first;
    return it->c_str();
}

static void register_method_locked(DarwinObjCClass *cls, const char *sel_name, void *imp) {
    if (!cls || !sel_name || !imp) return;
    const char *isel = intern_selector(sel_name);
    g_method_table[cls][isel] = imp;
    g_method_table_str[cls][sel_name] = imp;
}

static void register_instance_method(DarwinObjCClass *cls, const char *sel_name, void *imp) {
    std::lock_guard<std::mutex> lk(g_objc_mutex);
    register_method_locked(cls, sel_name, imp);
}

static void register_class_method(DarwinObjCClass *cls, const char *sel_name, void *imp) {
    std::lock_guard<std::mutex> lk(g_objc_mutex);
    if (cls && cls->isa) {
        register_method_locked(cls->isa, sel_name, imp);
    }
}

static const char *class_get_name_raw(const DarwinObjCClass *cls) {
    if (!cls) return "nil";
    const DarwinClassRO *ro = (const DarwinClassRO *)(uintptr_t)(cls->data_ptr & ~0x7ULL);
    if (ro && ro->name) return ro->name;
    return "UnknownClass";
}

static uint32_t class_get_instance_size_raw(const DarwinObjCClass *cls) {
    if (!cls) return 64;
    const DarwinClassRO *ro = (const DarwinClassRO *)(uintptr_t)(cls->data_ptr & ~0x7ULL);
    if (ro && ro->instanceSize > 0) return ro->instanceSize;
    return 64;
}

static bool is_valid_class_ptr(const void *p) {
    uintptr_t v = (uintptr_t)p;
    if (v < 0x10000ULL || (v & 0x7ULL) != 0) return false;
    return true;
}

static DarwinObjCPrefix *get_heap_prefix(const void *obj) {
    if (!obj) return nullptr;
    uintptr_t v = (uintptr_t)obj;
    if (v < 0x10000ULL || (v & 0x7ULL) != 0) return nullptr;
    const MachOImage *img = macho_get_active_image();
    if (img && img->base_addr) {
        uintptr_t base = (uintptr_t)img->base_addr;
        if (v >= base && v < base + img->mapped_span) {
            return nullptr; /* Inside static Mach-O segments */
        }
    }
    /* Check if obj is one of our static external class/metaclass/cfstring globals */
    DarwinObjCPrefix *pfx = ((DarwinObjCPrefix *)const_cast<void *>(obj)) - 1;
    if (pfx->magic == OH2_OBJC_HEAP_MAGIC) {
        return pfx;
    }
    return nullptr;
}

static void *alloc_objc_instance(DarwinObjCClass *cls, size_t extra_bytes = 0) {
    if (!cls) cls = &_OBJC_CLASS_$_NSObject;
    uint32_t inst_sz = class_get_instance_size_raw(cls);
    size_t payload_sz = std::max<size_t>(inst_sz + extra_bytes + 64, 512);
    payload_sz = (payload_sz + 15) & ~15ULL;
    size_t total_sz = sizeof(DarwinObjCPrefix) + payload_sz;

    uint8_t *raw = (uint8_t *)calloc(1, total_sz);
    if (!raw) return nullptr;

    DarwinObjCPrefix *pfx = (DarwinObjCPrefix *)raw;
    pfx->magic      = OH2_OBJC_HEAP_MAGIC;
    pfx->refcount   = 1;
    pfx->alloc_size = (uint32_t)payload_sz;

    void *obj = (void *)(raw + sizeof(DarwinObjCPrefix));
    *(DarwinObjCClass **)obj = cls;
    return obj;
}

static DarwinCFString *create_darwin_cfstring(const char *utf8, size_t len) {
    DarwinCFString *s = (DarwinCFString *)alloc_objc_instance(&_OBJC_CLASS_$_NSString, len + 32);
    if (!s) return nullptr;
    DarwinObjCPrefix *pfx = get_heap_prefix(s);
    pfx->cpp_str = new std::string(utf8 ? utf8 : "", len);
    s->isa         = &_OBJC_CLASS_$_NSString;
    s->flags       = 0x07c8U;
    s->dynamic_tag = 0x4F483253U; /* 'OH2S' */
    s->cstr        = pfx->cpp_str->c_str();
    s->length      = pfx->cpp_str->size();
    return s;
}

static DarwinCFString *create_darwin_cfstring_cstr(const char *utf8) {
    const char *safe = utf8 ? utf8 : "";
    return create_darwin_cfstring(safe, strlen(safe));
}

static void sync_mutable_cfstring(DarwinCFString *s) {
    DarwinObjCPrefix *pfx = get_heap_prefix(s);
    if (s && pfx && pfx->cpp_str) {
        s->cstr   = pfx->cpp_str->c_str();
        s->length = pfx->cpp_str->size();
    }
}

static uint64_t darwin_objc_fallback_imp(void *self, const char *sel) {
    (void)self;
    (void)sel;
    return 0;
}

} // namespace

extern "C" const char *darwin_cfstring_utf8(const void *cfstr_obj, char *scratch, size_t scratch_size) {
    if (!cfstr_obj) return "(null)";
    const DarwinObjCPrefix *pfx = get_heap_prefix(cfstr_obj);
    if (pfx && pfx->cpp_str) {
        return pfx->cpp_str->c_str();
    }
    const DarwinCFString *cfs = (const DarwinCFString *)cfstr_obj;
    /* Check if this is a static __DATA,__cfstring or Group-B DarwinCFString */
    if (cfs->isa == &___CFConstantStringClassReference ||
        cfs->isa == &_OBJC_CLASS_$_NSString ||
        cfs->isa == &_OBJC_CLASS_$_NSMutableString ||
        (cfs->flags & 0x07c0U) == 0x07c0U) {
        if ((cfs->flags & 0x10U) != 0 && cfs->cstr && scratch && scratch_size > 0) {
            /* UTF-16 __ustring constant */
            const uint16_t *u16 = (const uint16_t *)cfs->cstr;
            size_t n = (size_t)cfs->length;
            size_t out_i = 0;
            for (size_t i = 0; i < n && out_i + 1 < scratch_size; ++i) {
                uint16_t ch = u16[i];
                scratch[out_i++] = (ch < 0x80) ? (char)ch : '?';
            }
            scratch[out_i] = '\0';
            return scratch;
        }
        if (cfs->cstr) return cfs->cstr;
    }
    if (scratch && scratch_size > 0) {
        const DarwinObjCClass *cls = *(const DarwinObjCClass *const *)cfstr_obj;
        snprintf(scratch, scratch_size, "<%s:%p>",
                 is_valid_class_ptr(cls) ? class_get_name_raw(cls) : "Object",
                 cfstr_obj);
        return scratch;
    }
    return "<Object>";
}

static void *lookup_imp_from_class(const DarwinObjCClass *start_cls, const char *sel) {
    if (!start_cls || !sel) return (void *)&darwin_objc_fallback_imp;

    const DarwinObjCClass *cur = start_cls;
    int depth = 0;
    while (cur && is_valid_class_ptr(cur) && depth++ < 32) {
        auto it_cls = g_method_table.find(cur);
        if (it_cls != g_method_table.end()) {
            auto it_m = it_cls->second.find(sel);
            if (it_m != it_cls->second.end()) {
                return it_m->second;
            }
        }
        auto it_str_cls = g_method_table_str.find(cur);
        if (it_str_cls != g_method_table_str.end()) {
            auto it_sm = it_str_cls->second.find(sel);
            if (it_sm != it_str_cls->second.end()) {
                return it_sm->second;
            }
        }
        cur = cur->superclass;
    }

    uint64_t key = ((uint64_t)(uintptr_t)start_cls << 32) ^ (uint64_t)(uintptr_t)sel;
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lk(g_objc_mutex);
        if (g_logged_missing_sels.size() < 256 && g_logged_missing_sels.insert(key).second) {
            should_log = true;
        }
    }
    if (should_log) {
        const DarwinClassRO *ro = (const DarwinClassRO *)(uintptr_t)(start_cls->data_ptr & ~0x7ULL);
        bool is_meta = (ro && (ro->flags & 1U));
        oh2_log_info("[ObjC] Unimplemented selector %c[%s %s] -> returning 0",
                     is_meta ? '+' : '-',
                     class_get_name_raw(start_cls),
                     sel);
    }
    return (void *)&darwin_objc_fallback_imp;
}

extern "C" void *darwin_objc_lookup_imp(void *self, const char *sel) {
    if (!self || !sel) return (void *)&darwin_objc_fallback_imp;
    uintptr_t raw_isa = *(uintptr_t *)self;
    if ((raw_isa & 1ULL) != 0) {
        raw_isa &= 0x0000000ffffffff8ULL;
    }
    const DarwinObjCClass *cls = (const DarwinObjCClass *)raw_isa;
    if (!is_valid_class_ptr(cls)) {
        return (void *)&darwin_objc_fallback_imp;
    }
    return lookup_imp_from_class(cls, sel);
}

extern "C" void *darwin_objc_lookup_super2_imp(DarwinObjCSuper *sup, const char *sel) {
    if (!sup || !sup->receiver || !sup->current_class || !sel) {
        return (void *)&darwin_objc_fallback_imp;
    }
    const DarwinObjCClass *target_cls = sup->current_class->superclass;
    if (!target_cls) target_cls = sup->current_class;
    return lookup_imp_from_class(target_cls, sel);
}

/* ============================================================================
 * 4. Darwin -> Bionic Pthread, Mutex (0x32AAABA7), TLS & System Wrappers
 * ============================================================================ */

namespace {

static pthread_mutex_t g_darwin_sync_init_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *sanitize_darwin_mutex(void *m) {
    if (!m) return nullptr;
    uint32_t sig = __atomic_load_n((uint32_t *)m, __ATOMIC_ACQUIRE);
    if (__builtin_expect((sig & 0xfffffff0U) == 0x32aaaba0U, 0)) {
        pthread_mutex_lock(&g_darwin_sync_init_lock);
        sig = __atomic_load_n((uint32_t *)m, __ATOMIC_RELAXED);
        if ((sig & 0xfffffff0U) == 0x32aaaba0U) {
            memset(m, 0, 40);
            if (sig == 0x32aaaba2U) {
                pthread_mutexattr_t attr;
                pthread_mutexattr_init(&attr);
                pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
                pthread_mutex_init((pthread_mutex_t *)m, &attr);
                pthread_mutexattr_destroy(&attr);
            } else {
                pthread_mutex_init((pthread_mutex_t *)m, nullptr);
            }
        }
        pthread_mutex_unlock(&g_darwin_sync_init_lock);
    }
    return (pthread_mutex_t *)m;
}

static pthread_cond_t *sanitize_darwin_cond(void *c) {
    if (!c) return nullptr;
    uint32_t sig = __atomic_load_n((uint32_t *)c, __ATOMIC_ACQUIRE);
    if (__builtin_expect((sig & 0xfffffff0U) == 0x3cb0b1b0U, 0)) {
        pthread_mutex_lock(&g_darwin_sync_init_lock);
        sig = __atomic_load_n((uint32_t *)c, __ATOMIC_RELAXED);
        if ((sig & 0xfffffff0U) == 0x3cb0b1b0U) {
            memset(c, 0, 48);
            pthread_cond_init((pthread_cond_t *)c, nullptr);
        }
        pthread_mutex_unlock(&g_darwin_sync_init_lock);
    }
    return (pthread_cond_t *)c;
}

/* Darwin ARM64 144-byte struct stat */
struct DarwinStat64 {
    int32_t         st_dev;
    uint16_t        st_mode;
    uint16_t        st_nlink;
    uint64_t        st_ino;
    uint32_t        st_uid;
    uint32_t        st_gid;
    int32_t         st_rdev;
    int32_t         __pad0;
    struct timespec st_atimespec;
    struct timespec st_mtimespec;
    struct timespec st_ctimespec;
    struct timespec st_birthtimespec;
    int64_t         st_size;
    int64_t         st_blocks;
    int32_t         st_blksize;
    uint32_t        st_flags;
    uint32_t        st_gen;
    int32_t         st_lspare;
    int64_t         st_qspare[2];
};
static_assert(sizeof(DarwinStat64) == 144, "DarwinStat64 must be 144 bytes");

static void copy_bionic_to_darwin_stat(const struct stat &src, DarwinStat64 *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->st_dev           = (int32_t)src.st_dev;
    dst->st_mode          = (uint16_t)src.st_mode;
    dst->st_nlink         = (uint16_t)src.st_nlink;
    dst->st_ino           = (uint64_t)src.st_ino;
    dst->st_uid           = (uint32_t)src.st_uid;
    dst->st_gid           = (uint32_t)src.st_gid;
    dst->st_rdev          = (int32_t)src.st_rdev;
    dst->st_atimespec     = src.st_atim;
    dst->st_mtimespec     = src.st_mtim;
    dst->st_ctimespec     = src.st_ctim;
    dst->st_birthtimespec = src.st_ctim;
    dst->st_size          = (int64_t)src.st_size;
    dst->st_blocks        = (int64_t)src.st_blocks;
    dst->st_blksize       = (int32_t)src.st_blksize;
}

static int darwin_pthread_mutex_init(void *m, const pthread_mutexattr_t *attr) {
    if (!m) return EINVAL;
    memset(m, 0, 64);
    return pthread_mutex_init((pthread_mutex_t *)m, attr);
}

static int darwin_pthread_mutex_lock(void *m) {
    return pthread_mutex_lock(sanitize_darwin_mutex(m));
}

static int darwin_pthread_mutex_trylock(void *m) {
    return pthread_mutex_trylock(sanitize_darwin_mutex(m));
}

static int darwin_pthread_mutex_unlock(void *m) {
    return pthread_mutex_unlock(sanitize_darwin_mutex(m));
}

static int darwin_pthread_mutex_destroy(void *m) {
    if (!m) return 0;
    uint32_t sig = __atomic_load_n((uint32_t *)m, __ATOMIC_ACQUIRE);
    if ((sig & 0xfffffff0U) == 0x32aaaba0U) {
        memset(m, 0, 64);
        return 0;
    }
    return pthread_mutex_destroy((pthread_mutex_t *)m);
}

static int darwin_pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    return pthread_mutexattr_init(attr);
}

static int darwin_pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    return pthread_mutexattr_destroy(attr);
}

static int darwin_pthread_mutexattr_settype(pthread_mutexattr_t *attr, int darwin_type) {
    /* Darwin: NORMAL=0, ERRORCHECK=1, RECURSIVE=2. Bionic: NORMAL=0, RECURSIVE=1, ERRORCHECK=2 */
    int bionic_type = darwin_type;
    if (darwin_type == 2) bionic_type = PTHREAD_MUTEX_RECURSIVE;
    else if (darwin_type == 1) bionic_type = PTHREAD_MUTEX_ERRORCHECK;
    return pthread_mutexattr_settype(attr, bionic_type);
}

static int darwin_pthread_mutexattr_setprotocol(pthread_mutexattr_t *attr, int protocol) {
    (void)attr;
    (void)protocol;
    return 0;
}

static void darwin_std_mutex_lock(void *m) {
    darwin_pthread_mutex_lock(m);
}

static void darwin_std_mutex_unlock(void *m) {
    darwin_pthread_mutex_unlock(m);
}

static int darwin_pthread_cond_init(void *c, const pthread_condattr_t *attr) {
    if (!c) return EINVAL;
    memset(c, 0, 48);
    return pthread_cond_init((pthread_cond_t *)c, attr);
}

static int darwin_pthread_cond_wait(void *c, void *m) {
    return pthread_cond_wait(sanitize_darwin_cond(c), sanitize_darwin_mutex(m));
}

static int darwin_pthread_cond_timedwait(void *c, void *m, const struct timespec *abstime) {
    return pthread_cond_timedwait(sanitize_darwin_cond(c), sanitize_darwin_mutex(m), abstime);
}

static int darwin_pthread_cond_signal(void *c) {
    return pthread_cond_signal(sanitize_darwin_cond(c));
}

static int darwin_pthread_cond_broadcast(void *c) {
    return pthread_cond_broadcast(sanitize_darwin_cond(c));
}

static int darwin_pthread_cond_destroy(void *c) {
    if (!c) return 0;
    uint32_t sig = __atomic_load_n((uint32_t *)c, __ATOMIC_ACQUIRE);
    if ((sig & 0xfffffff0U) == 0x3cb0b1b0U) {
        memset(c, 0, 48);
        return 0;
    }
    return pthread_cond_destroy((pthread_cond_t *)c);
}

static int darwin_pthread_key_create(uint64_t *key_out, void (*destructor)(void *)) {
    if (!key_out) return EINVAL;
    pthread_key_t k = 0;
    int rc = pthread_key_create(&k, destructor);
    /* Zero-extend 32-bit Bionic key into full 64-bit Darwin pthread_key_t slot */
    *key_out = (rc == 0) ? (uint64_t)(uint32_t)k : 0ULL;
    return rc;
}

static int darwin_pthread_key_delete(uint64_t key) {
    return pthread_key_delete((pthread_key_t)(uint32_t)key);
}

static void *darwin_pthread_getspecific(uint64_t key) {
    return pthread_getspecific((pthread_key_t)(uint32_t)key);
}

static int darwin_pthread_setspecific(uint64_t key, const void *val) {
    return pthread_setspecific((pthread_key_t)(uint32_t)key, val);
}

static int darwin_pthread_setname_np(const char *name) {
    return pthread_setname_np(pthread_self(), name ? name : "OH2Thread");
}

static int darwin_pthread_cancel(pthread_t th) {
    (void)th;
    return 0;
}

static uint32_t darwin_pthread_mach_thread_np(pthread_t th) {
    (void)th;
    return (uint32_t)gettid();
}

static pthread_t darwin_pthread_from_mach_thread_np(uint32_t port) {
    (void)port;
    return pthread_self();
}

static int darwin_stat(const char *path, DarwinStat64 *d_st) {
    if (!path || !d_st) {
        errno = EINVAL;
        return -1;
    }
    struct stat b_st;
    if (stat(path, &b_st) != 0) return -1;
    copy_bionic_to_darwin_stat(b_st, d_st);
    return 0;
}

static int darwin_fstat(int fd, DarwinStat64 *d_st) {
    if (!d_st) {
        errno = EINVAL;
        return -1;
    }
    struct stat b_st;
    if (fstat(fd, &b_st) != 0) return -1;
    copy_bionic_to_darwin_stat(b_st, d_st);
    return 0;
}

static int *darwin_error(void) {
    return __errno();
}

static int darwin_maskrune(int c, unsigned long f) {
    if (c < 0 || c >= 256) return 0;
    return (int)(__DefaultRuneLocale.__runetype[c] & (uint32_t)f);
}

static int darwin_tolower(int c) {
    if (c < 0 || c >= 256) return tolower(c);
    return __DefaultRuneLocale.__maplower[c];
}

static void darwin_bzero(void *s, size_t n) {
    memset(s, 0, n);
}

static void *darwin_memchr(const void *s, int c, size_t n) {
    return const_cast<void *>(memchr(s, c, n));
}

static char *darwin_strchr(const char *s, int c) {
    return const_cast<char *>(strchr(s, c));
}

static char *darwin_strrchr(const char *s, int c) {
    return const_cast<char *>(strrchr(s, c));
}

static char *darwin_strstr(const char *h, const char *n) {
    return const_cast<char *>(strstr(h, n));
}

static void darwin_memset_pattern4(void *b, const void *pattern4, size_t len) {
    uint8_t *dst = (uint8_t *)b;
    for (size_t i = 0; i < len; ++i) {
        dst[i] = ((const uint8_t *)pattern4)[i & 3];
    }
}

static void darwin_memset_pattern8(void *b, const void *pattern8, size_t len) {
    uint8_t *dst = (uint8_t *)b;
    for (size_t i = 0; i < len; ++i) {
        dst[i] = ((const uint8_t *)pattern8)[i & 7];
    }
}

static void darwin_memset_pattern16(void *b, const void *pattern16, size_t len) {
    uint8_t *dst = (uint8_t *)b;
    for (size_t i = 0; i < len; ++i) {
        dst[i] = ((const uint8_t *)pattern16)[i & 15];
    }
}

static size_t darwin_malloc_size(const void *ptr) {
    if (!ptr) return 0;
    return malloc_usable_size(const_cast<void *>(ptr));
}

static double darwin_exp10(double x) {
    return pow(10.0, x);
}

static float darwin_exp10f(float x) {
    return powf(10.0f, x);
}

static int darwin_isfinitef(float x) {
    return isfinite(x) ? 1 : 0;
}

struct DoublePair { double s; double c; };
struct FloatPair  { float s; float c; };

static DoublePair darwin_sincos_stret(double x) {
    return {sin(x), cos(x)};
}

static FloatPair darwin_sincosf_stret(float x) {
    return {sinf(x), cosf(x)};
}

static void *darwin_memcpy_chk(void *dst, const void *src, size_t len, size_t dstlen) {
    (void)dstlen;
    return memcpy(dst, src, len);
}

static void *darwin_memmove_chk(void *dst, const void *src, size_t len, size_t dstlen) {
    (void)dstlen;
    return memmove(dst, src, len);
}

static void *darwin_memset_chk(void *dst, int c, size_t len, size_t dstlen) {
    (void)dstlen;
    return memset(dst, c, len);
}

static char *darwin_strcat_chk(char *dst, const char *src, size_t dstlen) {
    (void)dstlen;
    return strcat(dst, src);
}

static char *darwin_strncat_chk(char *dst, const char *src, size_t len, size_t dstlen) {
    (void)dstlen;
    return strncat(dst, src, len);
}

static int darwin_check_fd_set_overflow(int n, const void *fds, int unlimited) {
    (void)n;
    (void)fds;
    (void)unlimited;
    return 1;
}

static void darwin_assert_rtn(const char *func, const char *file, int line, const char *expr) {
    oh2_log_error("Darwin __assert_rtn(%s, %s:%d): %s",
                  func ? func : "?", file ? file : "?", line, expr ? expr : "?");
    abort();
}

static void darwin_stack_chk_fail(void) {
    oh2_log_error("Darwin __stack_chk_fail triggered");
    abort();
}

static int darwin_NSGetExecutablePath(char *buf, uint32_t *bufsize) {
    const MachOImage *img = macho_get_active_image();
    const char *p = (img && img->path[0]) ? img->path : "/data/local/tmp/OH2";
    size_t need = strlen(p) + 1;
    if (!bufsize) return -1;
    if (!buf || *bufsize < need) {
        *bufsize = (uint32_t)need;
        return -1;
    }
    memcpy(buf, p, need);
    return 0;
}

static uint64_t darwin_mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct DarwinMachTimebaseInfo {
    uint32_t numer;
    uint32_t denom;
};

static int darwin_mach_timebase_info(DarwinMachTimebaseInfo *info) {
    if (info) {
        info->numer = 1;
        info->denom = 1;
    }
    return 0;
}

static double darwin_CACurrentMediaTime(void) {
    return (double)darwin_mach_absolute_time() * 1e-9;
}

static int darwin_host_page_size(uint32_t host, uint64_t *out_page_size) {
    (void)host;
    if (out_page_size) *out_page_size = 4096;
    return 0;
}

static int darwin_host_statistics(uint32_t host, int flavor, int32_t *info_out, uint32_t *count_inout) {
    (void)host;
    (void)flavor;
    if (info_out && count_inout && *count_inout > 0) {
        /* Note: UE4 FIOSPlatformMemory::GetConstants passes sizeof(vm_statistics_data_t)=60
         * instead of HOST_VM_INFO_COUNT=15 words; clamp to 15 words (60 bytes) to prevent stack overflow. */
        uint32_t count = (*count_inout > 15U) ? 15U : *count_inout;
        memset(info_out, 0, (size_t)count * sizeof(int32_t));
        info_out[0] = 262144; /* free pages (~1 GB) */
        *count_inout = count;
    }
    return 0;
}

static int darwin_task_info(uint32_t task, int flavor, int64_t *info_out, uint32_t *count_inout) {
    (void)task;
    (void)flavor;
    if (info_out && count_inout && *count_inout > 0) {
        /* Clamp to MACH_TASK_BASIC_INFO_COUNT=12 words (48 bytes) */
        uint32_t count = (*count_inout > 12U) ? 12U : *count_inout;
        memset(info_out, 0, (size_t)count * sizeof(int32_t));
        *count_inout = count;
    }
    return 0;
}

static void *alloc_aligned_anon_mmap(size_t len, int prot, int linux_flags) {
    size_t map_len = (len > 0) ? ((len + 0xfffULL) & ~0xfffULL) : 4096ULL;
    const size_t align = 0x10000ULL; /* 64 KiB alignment satisfies iOS 16K/64K FMallocBinned2 pool alignment */
    void *raw = mmap(nullptr, map_len + align, prot, linux_flags, -1, 0);
    if (raw == MAP_FAILED) {
        return mmap(nullptr, map_len, prot, linux_flags, -1, 0);
    }
    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + (align - 1ULL)) & ~(align - 1ULL);
    size_t prefix = aligned_addr - raw_addr;
    size_t suffix = align - prefix;
    if (prefix > 0) munmap((void *)raw_addr, prefix);
    if (suffix > 0) munmap((void *)(aligned_addr + map_len), suffix);
    return (void *)aligned_addr;
}

static void *darwin_mmap(void *addr, size_t len, int prot, int darwin_flags, int fd, off_t offset) {
    int linux_flags = 0;
    if (darwin_flags & 0x0001) linux_flags |= MAP_SHARED;
    if (darwin_flags & 0x0002) linux_flags |= MAP_PRIVATE;
    if (darwin_flags & 0x0010) linux_flags |= MAP_FIXED;
    /* Darwin MAP_ANON = 0x1000, and VM_MAKE_TAG(tag) sets upper 8 bits of fd */
    if ((darwin_flags & 0x1000) || fd < 0 || ((uint32_t)fd >> 24) != 0) {
        linux_flags |= MAP_ANONYMOUS;
        if (!(linux_flags & (MAP_SHARED | MAP_PRIVATE))) linux_flags |= MAP_PRIVATE;
        if (addr == nullptr && !(linux_flags & MAP_FIXED)) {
            return alloc_aligned_anon_mmap(len, prot, linux_flags);
        }
        return mmap(addr, len ? len : 4096, prot, linux_flags, -1, 0);
    }
    if (!(linux_flags & (MAP_SHARED | MAP_PRIVATE))) linux_flags |= MAP_PRIVATE;
    return mmap(addr, len, prot, linux_flags, fd, offset);
}

static int darwin_posix_madvise(void *addr, size_t len, int advice) {
    (void)addr;
    (void)len;
    (void)advice;
    return 0;
}

static long darwin_sysconf(int darwin_name) {
    switch (darwin_name) {
        case 29: /* Darwin _SC_PAGESIZE / _SC_PAGE_SIZE */
            return 4096L;
        case 57: /* Darwin _SC_NPROCESSORS_CONF */
        case 58: /* Darwin _SC_NPROCESSORS_ONLN */ {
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            return (n > 0) ? n : 8L;
        }
        case 200: /* Darwin _SC_PHYS_PAGES */
            return (long)((6ULL * 1024ULL * 1024ULL * 1024ULL) / 4096ULL);
        default: {
            long r = sysconf(darwin_name);
            return (r >= 0) ? r : 4096L;
        }
    }
}

static int darwin_vm_allocate(uint32_t task, uintptr_t *addr, size_t size, int flags) {
    (void)task;
    if (!addr) return -1;
    bool anywhere = ((flags & 0x1) != 0) || (*addr == 0);
    void *m = anywhere
        ? alloc_aligned_anon_mmap(size ? size : 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS)
        : mmap((void *)(*addr), size ? size : 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return -1;
    *addr = (uintptr_t)m;
    return 0;
}

static int darwin_vm_deallocate(uint32_t task, uintptr_t addr, size_t size) {
    (void)task;
    if (addr && size) munmap((void *)addr, size);
    return 0;
}

static int darwin_vm_protect(uint32_t task, uintptr_t addr, size_t size, int set_max, int prot) {
    (void)task;
    (void)set_max;
    return mprotect((void *)addr, size, prot);
}

static int darwin_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen) {
    (void)newp;
    (void)newlen;
    if (!name) return -1;

    auto write_u32 = [&](uint32_t v) -> int {
        if (oldp && oldlenp && *oldlenp >= sizeof(uint32_t)) *(uint32_t *)oldp = v;
        if (oldlenp) *oldlenp = sizeof(uint32_t);
        return 0;
    };
    auto write_u64 = [&](uint64_t v) -> int {
        if (oldp && oldlenp && *oldlenp >= sizeof(uint64_t)) *(uint64_t *)oldp = v;
        else if (oldp && oldlenp && *oldlenp >= sizeof(uint32_t)) *(uint32_t *)oldp = (uint32_t)v;
        if (oldlenp) *oldlenp = sizeof(uint64_t);
        return 0;
    };
    auto write_str = [&](const char *s) -> int {
        size_t len = strlen(s) + 1;
        if (oldp && oldlenp && *oldlenp >= len) memcpy(oldp, s, len);
        if (oldlenp) *oldlenp = len;
        return 0;
    };

    if (strcmp(name, "hw.ncpu") == 0 ||
        strcmp(name, "hw.logicalcpu") == 0 ||
        strcmp(name, "hw.logicalcpu_max") == 0 ||
        strcmp(name, "hw.physicalcpu") == 0 ||
        strcmp(name, "hw.physicalcpu_max") == 0 ||
        strcmp(name, "hw.availcpu") == 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        return write_u32(ncpu > 0 ? (uint32_t)ncpu : 8U);
    }
    if (strcmp(name, "hw.memsize") == 0 || strcmp(name, "hw.physmem") == 0) {
        return write_u64(6ULL * 1024ULL * 1024ULL * 1024ULL);
    }
    if (strcmp(name, "hw.machine") == 0 || strcmp(name, "hw.model") == 0) {
        return write_str("iPhone12,1");
    }
    if (strcmp(name, "kern.osrelease") == 0) {
        return write_str("19.4.0");
    }
    if (strcmp(name, "kern.osversion") == 0) {
        return write_str("17E255");
    }
    if (strcmp(name, "kern.ostype") == 0) {
        return write_str("Darwin");
    }
    return write_u64(0);
}

static int darwin_sysctl(int *name, unsigned int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    (void)newp;
    (void)newlen;
    if (!name || namelen < 2) return -1;
    /* CTL_HW = 6: HW_NCPU = 3, HW_MEMSIZE = 24, HW_MACHINE = 1 */
    if (name[0] == 6 && (name[1] == 3 || name[1] == 25)) {
        return darwin_sysctlbyname("hw.ncpu", oldp, oldlenp, nullptr, 0);
    }
    if (name[0] == 6 && (name[1] == 24 || name[1] == 5)) {
        return darwin_sysctlbyname("hw.memsize", oldp, oldlenp, nullptr, 0);
    }
    if (name[0] == 6 && (name[1] == 1 || name[1] == 2)) {
        return darwin_sysctlbyname("hw.machine", oldp, oldlenp, nullptr, 0);
    }
    if (oldp && oldlenp && *oldlenp > 0) memset(oldp, 0, *oldlenp);
    return 0;
}

static int darwin_puts(const char *s) {
    if (s) {
        __android_log_print(ANDROID_LOG_INFO, "OH2_UE4", "%s", s);
        fprintf(stdout, "[OH2_UE4] %s\n", s);
        fflush(stdout);
    }
    return 0;
}

static int darwin_putchar(int c) {
    return putchar(c);
}

static int darwin_fputs(const char *s, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        darwin_puts(s);
        return 0;
    }
    return fputs(s ? s : "", stream);
}

static int darwin_fputc(int c, FILE *stream) {
    return fputc(c, stream);
}

static int32_t darwin_OSAtomicAdd32Barrier(int32_t amount, volatile int32_t *val) {
    return __atomic_add_fetch(val, amount, __ATOMIC_SEQ_CST);
}

static bool darwin_OSAtomicCompareAndSwap32Barrier(int32_t oldv, int32_t newv, volatile int32_t *val) {
    return __atomic_compare_exchange_n(val, &oldv, newv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static bool darwin_OSAtomicCompareAndSwap64Barrier(int64_t oldv, int64_t newv, volatile int64_t *val) {
    return __atomic_compare_exchange_n(val, &oldv, newv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static bool darwin_OSAtomicCompareAndSwapPtrBarrier(void *oldv, void *newv, void *volatile *val) {
    return __atomic_compare_exchange_n(val, &oldv, newv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void darwin_OSMemoryBarrier(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void darwin_OSSpinLockLock(volatile int32_t *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0) {
        sched_yield();
    }
}

static void darwin_OSSpinLockUnlock(volatile int32_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

static void darwin_uuid_generate(uint8_t out[16]) {
    if (!out) return;
    for (int i = 0; i < 16; ++i) out[i] = (uint8_t)(i * 17 + 0x42);
    out[6] = (out[6] & 0x0fU) | 0x40U;
    out[8] = (out[8] & 0x3fU) | 0x80U;
}

static uint8_t *darwin_CC_SHA256(const void *data, uint32_t len, uint8_t *md) {
    if (!md) return nullptr;
    memset(md, 0, 32);
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5U;
    for (uint32_t i = 0; i < len && p; ++i) {
        h ^= p[i];
        h *= 0x01000193U;
        md[i & 31] ^= (uint8_t)(h & 0xffU);
    }
    return md;
}

/* Blocks & Grand Central Dispatch M1 implementation */
struct DarwinBlockLiteral {
    void    *isa;
    int32_t  flags;
    int32_t  reserved;
    void   (*invoke)(void *, ...);
    struct {
        uintptr_t reserved;
        uintptr_t size;
    } *descriptor;
};

static void *darwin_Block_copy(const void *arg) {
    if (!arg) return nullptr;
    const DarwinBlockLiteral *b = (const DarwinBlockLiteral *)arg;
    if (b->isa == &__NSConcreteGlobalBlock) {
        return const_cast<void *>(arg);
    }
    size_t sz = (b->descriptor && b->descriptor->size >= sizeof(DarwinBlockLiteral) && b->descriptor->size < 65536)
                ? b->descriptor->size
                : 128;
    void *copy = malloc(sz);
    if (copy) memcpy(copy, arg, sz);
    return copy;
}

static void darwin_Block_release(const void *arg) {
    (void)arg;
}

static void darwin_Block_object_assign(void *dstAddr, const void *object, int flags) {
    (void)flags;
    if (dstAddr && object) *(const void **)dstAddr = object;
}

static void darwin_Block_object_dispose(const void *object, int flags) {
    (void)object;
    (void)flags;
}

static void darwin_dispatch_once_f(int64_t *predicate, void *context, void (*function)(void *)) {
    if (!predicate || !function) return;
    int64_t state = __atomic_load_n(predicate, __ATOMIC_ACQUIRE);
    if (__builtin_expect(state == ~0LL, 1)) return;
    pthread_mutex_lock(&g_darwin_sync_init_lock);
    if (__atomic_load_n(predicate, __ATOMIC_RELAXED) != ~0LL) {
        pthread_mutex_unlock(&g_darwin_sync_init_lock);
        function(context);
        __atomic_store_n(predicate, ~0LL, __ATOMIC_RELEASE);
        return;
    }
    pthread_mutex_unlock(&g_darwin_sync_init_lock);
}

static void *darwin_dispatch_get_global_queue(long identifier, unsigned long flags) {
    (void)identifier;
    (void)flags;
    return (void *)&__dispatch_main_q;
}

static void darwin_dispatch_async(void *queue, void *block) {
    (void)queue;
    if (!block) return;
    DarwinBlockLiteral *b = (DarwinBlockLiteral *)block;
    if (b->invoke) {
        b->invoke(b);
    }
}

struct DarwinDispatchSema {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    long            count;
};

static void *darwin_dispatch_semaphore_create(long value) {
    DarwinDispatchSema *s = (DarwinDispatchSema *)calloc(1, sizeof(DarwinDispatchSema));
    pthread_mutex_init(&s->mtx, nullptr);
    pthread_cond_init(&s->cv, nullptr);
    s->count = value;
    return s;
}

static long darwin_dispatch_semaphore_signal(void *dsema) {
    if (!dsema) return 0;
    DarwinDispatchSema *s = (DarwinDispatchSema *)dsema;
    pthread_mutex_lock(&s->mtx);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    return 0;
}

static long darwin_dispatch_semaphore_wait(void *dsema, uint64_t timeout) {
    if (!dsema) return 0;
    DarwinDispatchSema *s = (DarwinDispatchSema *)dsema;
    pthread_mutex_lock(&s->mtx);
    if (timeout == 0) {
        if (s->count > 0) {
            s->count--;
            pthread_mutex_unlock(&s->mtx);
            return 0;
        }
        pthread_mutex_unlock(&s->mtx);
        return 49; /* TIMEDOUT */
    }
    while (s->count <= 0) {
        pthread_cond_wait(&s->cv, &s->mtx);
    }
    s->count--;
    pthread_mutex_unlock(&s->mtx);
    return 0;
}

static void darwin_dispatch_retain(void *obj)  { (void)obj; }
static void darwin_dispatch_release(void *obj) { (void)obj; }

static bool darwin_availability_version_check(uint32_t count, const void *versions) {
    (void)count;
    (void)versions;
    return true;
}

static void *darwin_dlopen(const char *path, int mode) {
    (void)mode;
    if (!path) return (void *)0x1;
    void *h = dlopen(path, RTLD_LAZY);
    return h ? h : (void *)0x1;
}

static void *darwin_dlsym(void *handle, const char *symbol) {
    if (!symbol) return nullptr;
    /* Darwin RTLD_DEFAULT is (void*)-2, RTLD_NEXT is (void*)-1 */
    std::string underscored = std::string("_") + symbol;
    void *res = darwin_resolve_symbol(nullptr, underscored.c_str());
    if (!res) res = darwin_resolve_symbol(nullptr, symbol);
    if (!res && handle != (void *)(intptr_t)-2 && handle != (void *)(intptr_t)-1 && handle != (void *)0x1) {
        res = dlsym(handle, symbol);
    }
    if (!res) {
        res = dlsym(RTLD_DEFAULT, symbol);
    }
    return res;
}

static int darwin_dlclose(void *handle) {
    (void)handle;
    return 0;
}

static char *darwin_dlerror(void) {
    return nullptr;
}

static int darwin_dladdr(const void *addr, Dl_info *info) {
    const MachOImage *img = macho_get_active_image();
    if (img && img->base_addr && info) {
        uintptr_t v = (uintptr_t)addr;
        uintptr_t base = (uintptr_t)img->base_addr;
        if (v >= base && v < base + img->mapped_span) {
            info->dli_fname = img->path;
            info->dli_fbase = img->base_addr;
            info->dli_sname = "OH2_macho_symbol";
            info->dli_saddr = const_cast<void *>(addr);
            return 1;
        }
    }
    return dladdr(addr, info);
}

static int darwin_cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
    (void)func;
    (void)arg;
    (void)dso_handle;
    return 0;
}

static void darwin_dyld_stub_binder(void) {
    oh2_log_error("FATAL: dyld_stub_binder invoked unexpectedly (all 539 lazy_bind slots should be pre-bound)");
    abort();
}

} // namespace

/* ============================================================================
 * 5. CoreFoundation & libobjc Runtime Functions
 * ============================================================================ */

namespace {

static const void *darwin_CFRetain(const void *cf) {
    if (!cf) return nullptr;
    DarwinObjCPrefix *pfx = get_heap_prefix(cf);
    if (pfx) __atomic_add_fetch(&pfx->refcount, 1, __ATOMIC_RELAXED);
    return cf;
}

static void darwin_CFRelease(const void *cf) {
    (void)cf;
}

static uint64_t darwin_CFGetTypeID(const void *cf) {
    (void)cf;
    return 7ULL; /* CFString type ID */
}

static uint64_t darwin_CFStringGetTypeID(void) {
    return 7ULL;
}

static int64_t darwin_CFStringGetLength(const void *theString) {
    if (!theString) return 0;
    char scratch[256];
    const char *s = darwin_cfstring_utf8(theString, scratch, sizeof(scratch));
    return s ? (int64_t)strlen(s) : 0;
}

/*
 * Exact CoreFoundation CFStringGetBytes implementation supporting:
 * - Two-pass length query (buffer == NULL -> sets *usedBufLen and returns char count)
 * - kCFStringEncodingUTF16LE (0x14000100) / kCFStringEncodingUTF16 (0x00000100)
 * - kCFStringEncodingUTF8 (0x08000100) / ASCII (0x0600)
 */
static int64_t darwin_CFStringGetBytes(const void *theString,
                                       int64_t range_loc,
                                       int64_t range_len,
                                       uint32_t encoding,
                                       uint8_t lossByte,
                                       uint8_t isExternalRepresentation,
                                       uint8_t *buffer,
                                       int64_t maxBufLen,
                                       int64_t *usedBufLen) {
    (void)lossByte;
    (void)isExternalRepresentation;
    if (!theString || range_len <= 0 || range_loc < 0) {
        if (usedBufLen) *usedBufLen = 0;
        return 0;
    }

    char scratch[512];
    const char *utf8 = darwin_cfstring_utf8(theString, scratch, sizeof(scratch));
    if (!utf8) {
        if (usedBufLen) *usedBufLen = 0;
        return 0;
    }
    int64_t total_len = (int64_t)strlen(utf8);
    if (range_loc >= total_len) {
        if (usedBufLen) *usedBufLen = 0;
        return 0;
    }
    int64_t avail = std::min<int64_t>(range_len, total_len - range_loc);
    const char *src = utf8 + range_loc;

    bool is_utf16 = (encoding == 0x14000100U || encoding == 0x00000100U || encoding == 0x10000100U);
    if (is_utf16) {
        if (!buffer) {
            if (usedBufLen) *usedBufLen = avail * 2;
            return avail;
        }
        int64_t max_chars = (maxBufLen >= 0) ? (maxBufLen / 2) : avail;
        int64_t to_copy   = std::min<int64_t>(avail, max_chars);
        for (int64_t i = 0; i < to_copy; ++i) {
            uint16_t ch = (uint8_t)src[i];
            buffer[i * 2]     = (uint8_t)(ch & 0xffU);
            buffer[i * 2 + 1] = (uint8_t)((ch >> 8) & 0xffU);
        }
        if (usedBufLen) *usedBufLen = to_copy * 2;
        return to_copy;
    } else {
        if (!buffer) {
            if (usedBufLen) *usedBufLen = avail;
            return avail;
        }
        int64_t to_copy = (maxBufLen >= 0) ? std::min<int64_t>(avail, maxBufLen) : avail;
        memcpy(buffer, src, (size_t)to_copy);
        if (usedBufLen) *usedBufLen = to_copy;
        return to_copy;
    }
}

static void *darwin_CFStringCreateWithBytes(void *alloc,
                                            const uint8_t *bytes,
                                            int64_t numBytes,
                                            uint32_t encoding,
                                            uint8_t isExternalRepresentation) {
    (void)alloc;
    (void)isExternalRepresentation;
    if (!bytes || numBytes <= 0) return create_darwin_cfstring("", 0);

    bool is_utf16 = (encoding == 0x14000100U || encoding == 0x00000100U || encoding == 0x10000100U);
    if (is_utf16) {
        std::string s;
        int64_t chars = numBytes / 2;
        s.reserve((size_t)chars);
        for (int64_t i = 0; i < chars; ++i) {
            uint16_t ch = (uint16_t)bytes[i * 2] | ((uint16_t)bytes[i * 2 + 1] << 8);
            if (ch == 0) break;
            s.push_back(ch < 0x80 ? (char)ch : '?');
        }
        return create_darwin_cfstring(s.data(), s.size());
    }
    return create_darwin_cfstring((const char *)bytes, (size_t)numBytes);
}

static void *darwin_CFStringCreateWithCStringNoCopy(void *alloc,
                                                    const char *cStr,
                                                    uint32_t encoding,
                                                    void *contentsDeallocator) {
    (void)alloc;
    (void)encoding;
    (void)contentsDeallocator;
    return create_darwin_cfstring_cstr(cStr);
}

static uint8_t darwin_CFStringGetCString(const void *theString,
                                         char *buffer,
                                         int64_t bufferSize,
                                         uint32_t encoding) {
    (void)encoding;
    if (!theString || !buffer || bufferSize <= 0) return 0;
    char scratch[512];
    const char *s = darwin_cfstring_utf8(theString, scratch, sizeof(scratch));
    if (!s) return 0;
    snprintf(buffer, (size_t)bufferSize, "%s", s);
    return 1;
}

static void *darwin_CFArrayCreate(void *allocator, const void **values, int64_t numValues, const void *callBacks) {
    (void)allocator;
    (void)callBacks;
    void *arr = alloc_objc_instance(&_OBJC_CLASS_$_NSMutableArray);
    DarwinObjCPrefix *pfx = get_heap_prefix(arr);
    pfx->cpp_array = new std::vector<void *>();
    for (int64_t i = 0; i < numValues && values; ++i) {
        pfx->cpp_array->push_back(const_cast<void *>(values[i]));
    }
    return arr;
}

static int64_t darwin_CFArrayGetCount(const void *theArray) {
    const DarwinObjCPrefix *pfx = get_heap_prefix(theArray);
    if (pfx && pfx->cpp_array) return (int64_t)pfx->cpp_array->size();
    return 0;
}

static const void *darwin_CFArrayGetValueAtIndex(const void *theArray, int64_t idx) {
    const DarwinObjCPrefix *pfx = get_heap_prefix(theArray);
    if (pfx && pfx->cpp_array && idx >= 0 && (size_t)idx < pfx->cpp_array->size()) {
        return (*pfx->cpp_array)[(size_t)idx];
    }
    return nullptr;
}

static uint8_t darwin_CFBooleanGetValue(const void *boolean) {
    const DarwinObjCPrefix *pfx = get_heap_prefix(boolean);
    if (pfx) return pfx->num_i64 != 0 ? 1 : 0;
    return boolean != nullptr ? 1 : 0;
}

static void *darwin_CFDataCreateWithBytesNoCopy(void *allocator, const uint8_t *bytes, int64_t length, void *bytesDeallocator) {
    (void)allocator;
    (void)bytesDeallocator;
    void *d = alloc_objc_instance(&_OBJC_CLASS_$_NSData);
    DarwinObjCPrefix *pfx = get_heap_prefix(d);
    if (bytes && length > 0) {
        pfx->cpp_str = new std::string((const char *)bytes, (size_t)length);
    } else {
        pfx->cpp_str = new std::string();
    }
    return d;
}

static const uint8_t *darwin_CFDataGetBytePtr(const void *theData) {
    const DarwinObjCPrefix *pfx = get_heap_prefix(theData);
    if (pfx && pfx->cpp_str) return (const uint8_t *)pfx->cpp_str->data();
    return nullptr;
}

static int64_t darwin_CFDataGetLength(const void *theData) {
    const DarwinObjCPrefix *pfx = get_heap_prefix(theData);
    if (pfx && pfx->cpp_str) return (int64_t)pfx->cpp_str->size();
    return 0;
}

static const void *darwin_CFDictionaryGetValue(const void *theDict, const void *key) {
    const DarwinObjCPrefix *pfx = get_heap_prefix(theDict);
    if (!pfx || !pfx->cpp_dict || !key) return nullptr;
    char scratch[256];
    const char *k = darwin_cfstring_utf8(key, scratch, sizeof(scratch));
    if (!k) return nullptr;
    auto it = pfx->cpp_dict->find(k);
    return (it != pfx->cpp_dict->end()) ? it->second : nullptr;
}

static void *darwin_CFPropertyListCreateWithData(void *alloc, const void *data, uint64_t opts, uint64_t *fmt, void **err) {
    (void)alloc; (void)data; (void)opts; (void)fmt;
    if (err) *err = nullptr;
    void *dict = alloc_objc_instance(&_OBJC_CLASS_$_NSMutableDictionary);
    DarwinObjCPrefix *pfx = get_heap_prefix(dict);
    pfx->cpp_dict = new std::unordered_map<std::string, void *>();
    (*pfx->cpp_dict)["ProductVersion"] = create_darwin_cfstring_cstr("13.4");
    return dict;
}

static void *darwin_CFLocaleCopyCurrent(void) {
    return alloc_objc_instance(&_OBJC_CLASS_$_NSLocale);
}

static void *darwin_CFLocaleCopyPreferredLanguages(void) {
    const void *en = create_darwin_cfstring_cstr("en-US");
    return darwin_CFArrayCreate(nullptr, &en, 1, nullptr);
}

static const void *darwin_CFLocaleGetValue(const void *locale, const void *key) {
    (void)locale;
    char scratch[128];
    const char *k = darwin_cfstring_utf8(key, scratch, sizeof(scratch));
    if (k && strstr(k, "Country")) return create_darwin_cfstring_cstr("US");
    return create_darwin_cfstring_cstr("en");
}

struct DarwinCFUUIDBytes {
    uint8_t bytes[16];
};

static void *darwin_CFUUIDCreate(void *alloc) {
    (void)alloc;
    return create_darwin_cfstring_cstr("550E8400-E29B-41D4-A716-446655440000");
}

static void *darwin_CFUUIDCreateFromUUIDBytes(void *alloc, DarwinCFUUIDBytes bytes) {
    (void)alloc;
    (void)bytes;
    return create_darwin_cfstring_cstr("550E8400-E29B-41D4-A716-446655440000");
}

static void *darwin_CFUUIDCreateString(void *alloc, const void *uuid) {
    (void)alloc;
    (void)uuid;
    return create_darwin_cfstring_cstr("550E8400-E29B-41D4-A716-446655440000");
}

static DarwinCFUUIDBytes darwin_CFUUIDGetUUIDBytes(const void *uuid) {
    (void)uuid;
    DarwinCFUUIDBytes b;
    darwin_uuid_generate(b.bytes);
    return b;
}

/* libobjc.A.dylib functions */
static size_t darwin_class_getInstanceSize(DarwinObjCClass *cls) {
    return class_get_instance_size_raw(cls);
}

static void *darwin_class_getMethodImplementation(DarwinObjCClass *cls, const char *sel) {
    return lookup_imp_from_class(cls, sel);
}

static void *darwin_objc_alloc(DarwinObjCClass *cls) {
    return alloc_objc_instance(cls);
}

static void *darwin_objc_retain(void *obj) {
    if (!obj) return nullptr;
    DarwinObjCPrefix *pfx = get_heap_prefix(obj);
    if (pfx) __atomic_add_fetch(&pfx->refcount, 1, __ATOMIC_RELAXED);
    return obj;
}

static void darwin_objc_release(void *obj) {
    (void)obj;
}

static void *darwin_objc_autorelease(void *obj) {
    return obj;
}

static void *darwin_objc_autoreleasePoolPush(void) {
    return (void *)0x4F483250ULL; /* Pool sentinel */
}

static void darwin_objc_autoreleasePoolPop(void *pool) {
    (void)pool;
}

static void *darwin_objc_constructInstance(DarwinObjCClass *cls, void *bytes) {
    if (!cls || !bytes) return nullptr;
    *(DarwinObjCClass **)bytes = cls;
    return bytes;
}

static void *darwin_objc_destructInstance(void *obj) {
    return obj;
}

static void darwin_objc_copyCppObjectAtomic(void *dest, const void *src, void (*copyHelper)(void *dest, const void *source)) {
    std::lock_guard<std::mutex> lk(g_objc_mutex);
    if (copyHelper) copyHelper(dest, src);
}

static void darwin_objc_enumerationMutation(void *obj) {
    (void)obj;
}

static void *darwin_objc_getAssociatedObject(const void *object, const void *key) {
    std::lock_guard<std::mutex> lk(g_objc_mutex);
    auto it = g_assoc_objects.find((uint64_t)(uintptr_t)object);
    if (it == g_assoc_objects.end()) return nullptr;
    auto it2 = it->second.find(key);
    return (it2 != it->second.end()) ? it2->second : nullptr;
}

static void darwin_objc_setAssociatedObject(const void *object, const void *key, void *value, uintptr_t policy) {
    (void)policy;
    std::lock_guard<std::mutex> lk(g_objc_mutex);
    g_assoc_objects[(uint64_t)(uintptr_t)object][key] = value;
}

static void *darwin_objc_getProperty(void *self, const char *sel, ptrdiff_t offset, bool atomic) {
    (void)sel;
    (void)atomic;
    if (!self) return nullptr;
    return *(void **)((uint8_t *)self + offset);
}

static void darwin_objc_setProperty_atomic(void *self, const char *sel, void *newValue, ptrdiff_t offset) {
    (void)sel;
    if (!self) return;
    std::lock_guard<std::mutex> lk(g_objc_mutex);
    *(void **)((uint8_t *)self + offset) = newValue;
}

static void darwin_objc_setProperty_atomic_copy(void *self, const char *sel, void *newValue, ptrdiff_t offset) {
    darwin_objc_setProperty_atomic(self, sel, newValue, offset);
}

static void darwin_objc_setProperty_nonatomic(void *self, const char *sel, void *newValue, ptrdiff_t offset) {
    (void)sel;
    if (!self) return;
    *(void **)((uint8_t *)self + offset) = newValue;
}

static void darwin_objc_setProperty_nonatomic_copy(void *self, const char *sel, void *newValue, ptrdiff_t offset) {
    darwin_objc_setProperty_nonatomic(self, sel, newValue, offset);
}

static DarwinObjCClass *darwin_objc_getRequiredClass(const char *name) {
    if (!name) return &_OBJC_CLASS_$_NSObject;
    std::lock_guard<std::mutex> lk(g_objc_mutex);
    auto it = g_class_by_name.find(name);
    return (it != g_class_by_name.end()) ? it->second : &_OBJC_CLASS_$_NSObject;
}

static DarwinObjCClass *darwin_object_getClass(void *obj) {
    if (!obj) return nullptr;
    return *(DarwinObjCClass **)obj;
}

static DarwinObjCClass *darwin_object_setClass(void *obj, DarwinObjCClass *cls) {
    if (!obj) return nullptr;
    DarwinObjCClass *old = *(DarwinObjCClass **)obj;
    *(DarwinObjCClass **)obj = cls;
    return old;
}

static int darwin_objc_sync_enter(void *obj) {
    (void)obj;
    return 0;
}

static int darwin_objc_sync_exit(void *obj) {
    (void)obj;
    return 0;
}

} // namespace

/* ============================================================================
 * 6. Foundation & UIKit Objective-C Class Implementations for M1 Bootstrap
 * ============================================================================ */

namespace {

static void *g_shared_application = nullptr;
static void *g_main_bundle        = nullptr;
static void *g_main_screen        = nullptr;
static void *g_current_device     = nullptr;
static void *g_process_info       = nullptr;
static void *g_notif_center       = nullptr;
static void *g_user_notif_center  = nullptr;
static void *g_audio_session      = nullptr;
static void *g_file_manager       = nullptr;
static void *g_user_defaults      = nullptr;

/* NSObject core methods */
static void *imp_nsobject_alloc(DarwinObjCClass *cls, const char *sel) {
    (void)sel;
    return alloc_objc_instance(cls);
}
static void *imp_nsobject_allocWithZone(DarwinObjCClass *cls, const char *sel, void *zone) {
    (void)sel; (void)zone;
    return alloc_objc_instance(cls);
}
static void *imp_nsobject_init(void *self, const char *sel) {
    (void)sel;
    return self;
}
static void *imp_nsobject_new(DarwinObjCClass *cls, const char *sel) {
    void *obj = alloc_objc_instance(cls);
    if (!obj) return nullptr;
    const char *init_sel = intern_selector("init");
    void *(*init_fn)(void *, const char *) = (void *(*)(void *, const char *))darwin_objc_lookup_imp(obj, init_sel);
    return init_fn ? init_fn(obj, init_sel) : obj;
}
static void *imp_nsobject_self(void *self, const char *sel) { (void)sel; return self; }
static void *imp_nsobject_retain(void *self, const char *sel) { (void)sel; return darwin_objc_retain(self); }
static void  imp_nsobject_release(void *self, const char *sel) { (void)sel; }
static void *imp_nsobject_autorelease(void *self, const char *sel) { (void)sel; return self; }
static void  imp_nsobject_dealloc(void *self, const char *sel) { (void)self; (void)sel; }
static void *imp_nsobject_copy(void *self, const char *sel) { (void)sel; return self; }
static void *imp_nsobject_class_inst(void *self, const char *sel) {
    (void)sel;
    return self ? *(DarwinObjCClass **)self : nullptr;
}
static void *imp_nsobject_class_meta(DarwinObjCClass *cls, const char *sel) {
    (void)sel;
    return cls;
}
static void *imp_nsobject_superclass(void *self, const char *sel) {
    (void)sel;
    if (!self) return nullptr;
    DarwinObjCClass *cls = *(DarwinObjCClass **)self;
    return cls ? cls->superclass : nullptr;
}
static uint8_t imp_nsobject_respondsToSelector(void *self, const char *sel, const char *query_sel) {
    (void)sel;
    if (!self || !query_sel) return 0;
    return 1;
}
static uint8_t imp_nsobject_isKindOfClass(void *self, const char *sel, DarwinObjCClass *target_cls) {
    (void)sel;
    if (!self || !target_cls) return 0;
    const DarwinObjCClass *cur = *(const DarwinObjCClass **)self;
    int depth = 0;
    while (cur && is_valid_class_ptr(cur) && depth++ < 32) {
        if (cur == target_cls) return 1;
        cur = cur->superclass;
    }
    return 0;
}
static void *imp_nsobject_description(void *self, const char *sel) {
    (void)sel;
    char scratch[256];
    const char *s = darwin_cfstring_utf8(self, scratch, sizeof(scratch));
    return create_darwin_cfstring_cstr(s);
}

/* NSString & NSMutableString methods */
static uint64_t imp_nsstring_length(void *self, const char *sel) {
    (void)sel;
    return (uint64_t)darwin_CFStringGetLength(self);
}
static const char *imp_nsstring_UTF8String(void *self, const char *sel) {
    (void)sel;
    static thread_local char tl_scratch[512];
    return darwin_cfstring_utf8(self, tl_scratch, sizeof(tl_scratch));
}
static void *imp_nsstring_string(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    return create_darwin_cfstring("", 0);
}
static void *imp_nsstring_stringWithUTF8String(DarwinObjCClass *cls, const char *sel, const char *cstr) {
    (void)cls; (void)sel;
    return create_darwin_cfstring_cstr(cstr);
}
static void *imp_nsstring_initWithUTF8String(void *self, const char *sel, const char *cstr) {
    (void)sel;
    DarwinCFString *s = (DarwinCFString *)self;
    DarwinObjCPrefix *pfx = get_heap_prefix(s);
    if (pfx) {
        delete pfx->cpp_str;
        pfx->cpp_str = new std::string(cstr ? cstr : "");
        s->flags = 0x07c8U;
        sync_mutable_cfstring(s);
        return s;
    }
    return create_darwin_cfstring_cstr(cstr);
}
static void *imp_nsstring_initWithCString_enc(void *self, const char *sel, const char *cstr, uint64_t enc) {
    (void)enc;
    return imp_nsstring_initWithUTF8String(self, sel, cstr);
}
static void *imp_nsstring_stringWithCString_enc(DarwinObjCClass *cls, const char *sel, const char *cstr, uint64_t enc) {
    (void)cls; (void)sel; (void)enc;
    return create_darwin_cfstring_cstr(cstr);
}
static void *imp_nsstring_stringWithString(DarwinObjCClass *cls, const char *sel, void *other) {
    (void)cls; (void)sel;
    char scratch[512];
    return create_darwin_cfstring_cstr(darwin_cfstring_utf8(other, scratch, sizeof(scratch)));
}
static void *imp_nsstring_initWithString(void *self, const char *sel, void *other) {
    char scratch[512];
    return imp_nsstring_initWithUTF8String(self, sel, darwin_cfstring_utf8(other, scratch, sizeof(scratch)));
}
static void *imp_nsstring_initWithContentsOfFile(void *self, const char *sel, void *path) {
    (void)self; (void)sel; (void)path;
    return nullptr;
}
static void *imp_nsstring_stringByAppendingString(void *self, const char *sel, void *other) {
    (void)sel;
    char s1[512], s2[512];
    std::string res = darwin_cfstring_utf8(self, s1, sizeof(s1));
    res += darwin_cfstring_utf8(other, s2, sizeof(s2));
    return create_darwin_cfstring(res.data(), res.size());
}
static void *imp_nsstring_stringByAppendingPathComponent(void *self, const char *sel, void *comp) {
    (void)sel;
    char s1[512], s2[512];
    std::string base = darwin_cfstring_utf8(self, s1, sizeof(s1));
    std::string add  = darwin_cfstring_utf8(comp, s2, sizeof(s2));
    if (!base.empty() && base.back() != '/' && (add.empty() || add.front() != '/')) {
        base.push_back('/');
    }
    base += add;
    return create_darwin_cfstring(base.data(), base.size());
}
static void imp_nsmutablestring_setString(void *self, const char *sel, void *other) {
    (void)sel;
    DarwinCFString *s = (DarwinCFString *)self;
    DarwinObjCPrefix *pfx = get_heap_prefix(s);
    if (!pfx) return;
    char scratch[512];
    const char *o = darwin_cfstring_utf8(other, scratch, sizeof(scratch));
    if (!pfx->cpp_str) pfx->cpp_str = new std::string(o ? o : "");
    else *pfx->cpp_str = (o ? o : "");
    s->flags = 0x07c8U;
    sync_mutable_cfstring(s);
}
static void imp_nsmutablestring_appendString(void *self, const char *sel, void *other) {
    (void)sel;
    DarwinCFString *s = (DarwinCFString *)self;
    DarwinObjCPrefix *pfx = get_heap_prefix(s);
    if (!pfx) return;
    char scratch[512];
    const char *o = darwin_cfstring_utf8(other, scratch, sizeof(scratch));
    if (!pfx->cpp_str) pfx->cpp_str = new std::string();
    if (o) *pfx->cpp_str += o;
    s->flags = 0x07c8U;
    sync_mutable_cfstring(s);
}
static float imp_nsstring_floatValue(void *self, const char *sel) {
    (void)sel;
    char scratch[128];
    const char *s = darwin_cfstring_utf8(self, scratch, sizeof(scratch));
    return s ? strtof(s, nullptr) : 0.0f;
}
static double imp_nsstring_doubleValue(void *self, const char *sel) {
    (void)sel;
    char scratch[128];
    const char *s = darwin_cfstring_utf8(self, scratch, sizeof(scratch));
    return s ? strtod(s, nullptr) : 0.0;
}
static int32_t imp_nsstring_intValue(void *self, const char *sel) {
    (void)sel;
    char scratch[128];
    const char *s = darwin_cfstring_utf8(self, scratch, sizeof(scratch));
    return s ? (int32_t)strtol(s, nullptr, 10) : 0;
}
static uint8_t imp_nsstring_isEqualToString(void *self, const char *sel, void *other) {
    (void)sel;
    if (self == other) return 1;
    if (!self || !other) return 0;
    char s1[512], s2[512];
    return strcmp(darwin_cfstring_utf8(self, s1, sizeof(s1)),
                  darwin_cfstring_utf8(other, s2, sizeof(s2))) == 0 ? 1 : 0;
}

} // namespace

extern "C" void *darwin_objc_stringWithFormat_impl(void *self, const char *sel, const void *ns_fmt, char *darwin_sp) {
    (void)self;
    (void)sel;
    char fmt_scratch[512];
    const char *fmt = darwin_cfstring_utf8(ns_fmt, fmt_scratch, sizeof(fmt_scratch));
    char out_buf[2048];
    darwin_vsnprintf_objc(out_buf, sizeof(out_buf), fmt ? fmt : "", darwin_sp);
    return create_darwin_cfstring_cstr(out_buf);
}

extern "C" void darwin_objc_appendFormat_impl(void *self, const char *sel, const void *ns_fmt, char *darwin_sp) {
    void *formatted = darwin_objc_stringWithFormat_impl(self, sel, ns_fmt, darwin_sp);
    imp_nsmutablestring_appendString(self, sel, formatted);
}

namespace {

/* NSBundle */
static void *imp_nsbundle_mainBundle(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_main_bundle) {
        g_main_bundle = alloc_objc_instance(&_OBJC_CLASS_$_NSBundle);
    }
    return g_main_bundle;
}
static void *imp_nsbundle_bundlePath(void *self, const char *sel) {
    (void)self; (void)sel;
    return create_darwin_cfstring_cstr("/data/local/tmp/OH2.app");
}
static void *imp_nsbundle_resourcePath(void *self, const char *sel) {
    (void)self; (void)sel;
    return create_darwin_cfstring_cstr("/data/local/tmp/OH2.app");
}
static void *imp_nsbundle_executablePath(void *self, const char *sel) {
    (void)self; (void)sel;
    const MachOImage *img = macho_get_active_image();
    return create_darwin_cfstring_cstr((img && img->path[0]) ? img->path : "/data/local/tmp/OH2");
}
static void *imp_nsbundle_bundleIdentifier(void *self, const char *sel) {
    (void)self; (void)sel;
    return create_darwin_cfstring_cstr("com.cornfox.oceanhorn2");
}
static void *imp_nsbundle_objectForInfoDictionaryKey(void *self, const char *sel, void *key) {
    (void)self; (void)sel;
    char scratch[128];
    const char *k = darwin_cfstring_utf8(key, scratch, sizeof(scratch));
    if (k && strcmp(k, "CFBundleIdentifier") == 0) return create_darwin_cfstring_cstr("com.cornfox.oceanhorn2");
    if (k && strcmp(k, "CFBundleVersion") == 0) return create_darwin_cfstring_cstr("1.5");
    if (k && strcmp(k, "CFBundleShortVersionString") == 0) return create_darwin_cfstring_cstr("1.5");
    return create_darwin_cfstring_cstr("OH2");
}
static void *imp_nsbundle_infoDictionary(void *self, const char *sel) {
    (void)self; (void)sel;
    void *d = alloc_objc_instance(&_OBJC_CLASS_$_NSMutableDictionary);
    DarwinObjCPrefix *pfx = get_heap_prefix(d);
    pfx->cpp_dict = new std::unordered_map<std::string, void *>();
    (*pfx->cpp_dict)["CFBundleIdentifier"] = create_darwin_cfstring_cstr("com.cornfox.oceanhorn2");
    (*pfx->cpp_dict)["CFBundleVersion"]    = create_darwin_cfstring_cstr("1.5");
    return d;
}

/* UIDevice */
static void *imp_uidevice_currentDevice(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_current_device) g_current_device = alloc_objc_instance(&_OBJC_CLASS_$_UIDevice);
    return g_current_device;
}
static void *imp_uidevice_systemVersion(void *self, const char *sel) {
    (void)self; (void)sel;
    return create_darwin_cfstring_cstr("13.4");
}
static void *imp_uidevice_model(void *self, const char *sel) {
    (void)self; (void)sel;
    return create_darwin_cfstring_cstr("iPhone");
}
static float imp_uidevice_batteryLevel(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1.0f;
}
static int64_t imp_uidevice_batteryState(void *self, const char *sel) {
    (void)self; (void)sel;
    return 2; /* UIDeviceBatteryStateCharging */
}

/* UIScreen */
static void *imp_uiscreen_mainScreen(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_main_screen) {
        g_main_screen = alloc_objc_instance(&_OBJC_CLASS_$_UIScreen);
        DarwinObjCPrefix *pfx = get_heap_prefix(g_main_screen);
        pfx->frame = {0.0, 0.0, 2340.0, 1080.0};
    }
    return g_main_screen;
}
static CGRect64 imp_uiscreen_bounds(void *self, const char *sel) {
    (void)self; (void)sel;
    return {0.0, 0.0, 2340.0, 1080.0};
}
static double imp_uiscreen_scale(void *self, const char *sel) {
    (void)self; (void)sel;
    return 2.0;
}

/* UIApplication */
static void *imp_uiapplication_sharedApplication(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_shared_application) g_shared_application = alloc_objc_instance(&_OBJC_CLASS_$_UIApplication);
    return g_shared_application;
}
static int64_t imp_uiapplication_statusBarOrientation(void *self, const char *sel) {
    (void)self; (void)sel;
    return 3; /* UIInterfaceOrientationLandscapeRight */
}
static void *imp_uiapplication_delegate(void *self, const char *sel) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    return pfx ? pfx->delegate : nullptr;
}
static void imp_uiapplication_setDelegate(void *self, const char *sel, void *del) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx) pfx->delegate = del;
}

/* UIView / UIWindow */
static void *imp_uiview_initWithFrame(void *self, const char *sel, double x, double y, double w, double h) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx) pfx->frame = {x, y, w, h};
    return self;
}
static CGRect64 imp_uiview_frame(void *self, const char *sel) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx && pfx->frame.width > 0) return pfx->frame;
    return {0.0, 0.0, 2340.0, 1080.0};
}
static void imp_uiview_setFrame(void *self, const char *sel, double x, double y, double w, double h) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx) pfx->frame = {x, y, w, h};
}
static void *imp_uiview_layer(void *self, const char *sel) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx) {
        if (!pfx->layer) pfx->layer = alloc_objc_instance(&_OBJC_CLASS_$_CAMetalLayer);
        return pfx->layer;
    }
    return alloc_objc_instance(&_OBJC_CLASS_$_CAMetalLayer);
}

/* NSProcessInfo */
static void *imp_nsprocessinfo_processInfo(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_process_info) g_process_info = alloc_objc_instance(&_OBJC_CLASS_$_NSProcessInfo);
    return g_process_info;
}
static int64_t imp_nsprocessinfo_thermalState(void *self, const char *sel) {
    (void)self; (void)sel;
    return 0; /* NSProcessInfoThermalStateNominal */
}

/* NSNotificationCenter & UNUserNotificationCenter & AVAudioSession & NSFileManager & NSUserDefaults */
static void *imp_nsnotif_defaultCenter(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_notif_center) g_notif_center = alloc_objc_instance(&_OBJC_CLASS_$_NSNotificationCenter);
    return g_notif_center;
}
static void *imp_unuser_currentNotificationCenter(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_user_notif_center) g_user_notif_center = alloc_objc_instance(&_OBJC_CLASS_$_UNUserNotificationCenter);
    return g_user_notif_center;
}
static void *imp_avaudiosession_sharedInstance(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_audio_session) g_audio_session = alloc_objc_instance(&_OBJC_CLASS_$_AVAudioSession);
    return g_audio_session;
}
static uint8_t imp_avaudiosession_true(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1;
}
static float imp_avaudiosession_outputVolume(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1.0f;
}
static void *imp_nsfilemanager_defaultManager(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_file_manager) g_file_manager = alloc_objc_instance(&_OBJC_CLASS_$_NSFileManager);
    return g_file_manager;
}
static void *imp_nsuserdefaults_standardUserDefaults(DarwinObjCClass *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_user_defaults) g_user_defaults = alloc_objc_instance(&_OBJC_CLASS_$_NSUserDefaults);
    return g_user_defaults;
}
static void *imp_nstimer_scheduledTimer(DarwinObjCClass *cls, const char *sel,
                                        double interval, void *target, const char *aSelector,
                                        void *userInfo, uint8_t repeats) {
    (void)cls; (void)sel; (void)interval; (void)repeats;
    void *t = alloc_objc_instance(&_OBJC_CLASS_$_NSTimer);
    DarwinObjCPrefix *pfx = get_heap_prefix(t);
    if (pfx) {
        pfx->target        = target;
        pfx->selector_name = aSelector;
        pfx->user_info     = userInfo;
    }
    return t;
}
static void *imp_nsthread_initWithTarget(void *self, const char *sel,
                                         void *target, const char *aSelector, void *arg) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx) {
        pfx->target        = target;
        pfx->selector_name = aSelector;
        pfx->user_info     = arg;
    }
    return self;
}
static void imp_nsthread_start(void *self, const char *sel) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    oh2_log_info("NSThread start requested (target=%p selector=%s) — deferred in M1 headless mode",
                 pfx ? pfx->target : nullptr,
                 (pfx && pfx->selector_name) ? pfx->selector_name : "?");
}

/* NSArray / NSMutableArray */
static void *imp_nsarray_array(DarwinObjCClass *cls, const char *sel) {
    (void)sel;
    void *a = alloc_objc_instance(cls);
    DarwinObjCPrefix *pfx = get_heap_prefix(a);
    pfx->cpp_array = new std::vector<void *>();
    return a;
}
static void *imp_nsarray_init(void *self, const char *sel) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx && !pfx->cpp_array) pfx->cpp_array = new std::vector<void *>();
    return self;
}
static uint64_t imp_nsarray_count(void *self, const char *sel) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    return (pfx && pfx->cpp_array) ? (uint64_t)pfx->cpp_array->size() : 0ULL;
}
static void *imp_nsarray_objectAtIndex(void *self, const char *sel, uint64_t idx) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx && pfx->cpp_array && idx < pfx->cpp_array->size()) return (*pfx->cpp_array)[idx];
    return nullptr;
}
static void imp_nsmutablearray_addObject(void *self, const char *sel, void *obj) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (pfx && obj) {
        if (!pfx->cpp_array) pfx->cpp_array = new std::vector<void *>();
        pfx->cpp_array->push_back(obj);
    }
}

/* NSDictionary / NSMutableDictionary */
static void *imp_nsdict_dictionary(DarwinObjCClass *cls, const char *sel) {
    (void)sel;
    void *d = alloc_objc_instance(cls);
    DarwinObjCPrefix *pfx = get_heap_prefix(d);
    pfx->cpp_dict = new std::unordered_map<std::string, void *>();
    return d;
}
static void *imp_nsdict_objectForKey(void *self, const char *sel, void *key) {
    (void)sel;
    return const_cast<void *>(darwin_CFDictionaryGetValue(self, key));
}
static void imp_nsmutabledict_setObjectForKey(void *self, const char *sel, void *obj, void *key) {
    (void)sel;
    DarwinObjCPrefix *pfx = get_heap_prefix(self);
    if (!pfx || !key || !obj) return;
    if (!pfx->cpp_dict) pfx->cpp_dict = new std::unordered_map<std::string, void *>();
    char scratch[256];
    const char *k = darwin_cfstring_utf8(key, scratch, sizeof(scratch));
    if (k) (*pfx->cpp_dict)[k] = obj;
}

/* Foundation C Functions */
static void *darwin_NSStringFromClass(DarwinObjCClass *cls) {
    return create_darwin_cfstring_cstr(class_get_name_raw(cls));
}

static void *darwin_NSStringFromSelector(const char *sel) {
    return create_darwin_cfstring_cstr(sel ? sel : "");
}

static void *darwin_NSHomeDirectory(void) {
    return create_darwin_cfstring_cstr("/data/local/tmp");
}

static void *darwin_NSTemporaryDirectory(void) {
    return create_darwin_cfstring_cstr("/data/local/tmp");
}

static void *darwin_NSSearchPathForDirectoriesInDomains(uint64_t dir, uint64_t domainMask, uint8_t expandTilde) {
    (void)dir; (void)domainMask; (void)expandTilde;
    const void *p = create_darwin_cfstring_cstr("/data/local/tmp");
    return darwin_CFArrayCreate(nullptr, &p, 1, nullptr);
}

static void darwin_NSSetUncaughtExceptionHandler(void *handler) {
    (void)handler;
}

/*
 * UIApplicationMain — M1 Headless Bootstrap Entry Point:
 * Instantiates OH2's IOSAppDelegate, invokes -[IOSAppDelegate application:didFinishLaunchingWithOptions:],
 * invokes -[IOSAppDelegate NoUrlCommandLine], and calls OH2's FIOSPlatformMisc::LowLevelOutputDebugString
 * so genuine UE4 startup code & log sinks inside OH2 execute and emit to logcat before exiting 0.
 */
static int darwin_UIApplicationMain(int argc, char **argv, void *principalClassName, void *delegateClassName) {
    (void)argc;
    (void)argv;
    (void)principalClassName;

    char scratch[128];
    const char *del_name = darwin_cfstring_utf8(delegateClassName, scratch, sizeof(scratch));
    if (!del_name || !del_name[0] || strcmp(del_name, "(null)") == 0) {
        del_name = "IOSAppDelegate";
    }
    oh2_log_info("UIApplicationMain entered: delegateClassName='%s'", del_name);

    void *app = imp_uiapplication_sharedApplication(&_OBJC_CLASS_$_UIApplication, "sharedApplication");
    DarwinObjCClass *del_cls = darwin_objc_getRequiredClass(del_name);
    void *app_delegate = imp_nsobject_new(del_cls, "new");
    imp_uiapplication_setDelegate(app, "setDelegate:", app_delegate);

    /* Invoke -[IOSAppDelegate application:didFinishLaunchingWithOptions:] inside OH2 */
    const char *did_finish_sel = intern_selector("application:didFinishLaunchingWithOptions:");
    typedef uint8_t (*did_finish_fn_t)(void *, const char *, void *, void *);
    did_finish_fn_t did_finish_fn = (did_finish_fn_t)darwin_objc_lookup_imp(app_delegate, did_finish_sel);
    uint8_t ok = 0;
    if (did_finish_fn && (void *)did_finish_fn != (void *)&darwin_objc_fallback_imp) {
        oh2_log_info("Invoking -[IOSAppDelegate application:didFinishLaunchingWithOptions:] at %p",
                     (void *)did_finish_fn);
        ok = did_finish_fn(app_delegate, did_finish_sel, app, nullptr);
        oh2_log_info("-[IOSAppDelegate application:didFinishLaunchingWithOptions:] returned %u", (unsigned)ok);
    }

    /* Invoke -[IOSAppDelegate NoUrlCommandLine] inside OH2 */
    const char *no_url_sel = intern_selector("NoUrlCommandLine");
    typedef void (*no_url_fn_t)(void *, const char *);
    no_url_fn_t no_url_fn = (no_url_fn_t)darwin_objc_lookup_imp(app_delegate, no_url_sel);
    if (no_url_fn && (void *)no_url_fn != (void *)&darwin_objc_fallback_imp) {
        no_url_fn(app_delegate, no_url_sel);
    }

    /* Invoke OH2's FIOSPlatformMisc::LowLevelOutputDebugString (0x100aae928) -> NSLog(@"[UE4] %s") */
    const MachOImage *img = macho_get_active_image();
    if (img && img->base_addr) {
        typedef void (*ue4_low_level_debug_str_fn_t)(const char16_t *);
        ue4_low_level_debug_str_fn_t ue4_log_fn =
            (ue4_low_level_debug_str_fn_t)((uintptr_t)img->base_addr + 0xaae928ULL);
        static const char16_t kUe4BootMsg[] =
            u"LogInit: Oceanhorn 2 UE4.23 Mach-O M1 Runtime Active on Android ARM64";
        ue4_log_fn(kUe4BootMsg);
    }

    oh2_log_info("UIApplicationMain M1 headless verification complete; returning 0 to LC_MAIN");
    return 0;
}

} // namespace

/* ============================================================================
 * 7. Complete 805-Symbol Resolver Table & Diagnostic Stubs
 * ============================================================================ */

namespace {

static std::unordered_map<std::string, void *> g_symbol_table;

/* Per-symbol diagnostic stub for deferred M2/M5 framework functions (OpenGLES, AudioToolbox, etc.) */
#define DEFINE_DIAGNOSTIC_STUB(fn_name) \
    static uint64_t stub_##fn_name(void) { \
        static std::atomic<uint32_t> s_calls{0}; \
        uint32_t c = s_calls.fetch_add(1, std::memory_order_relaxed); \
        if (c < 3) { \
            oh2_log_info("[STUB] " #fn_name " called"); \
        } \
        return 0; \
    }

OH2_FOR_EACH_542_FUNCTION(DEFINE_DIAGNOSTIC_STUB)
#undef DEFINE_DIAGNOSTIC_STUB

static void init_rune_locale(void) {
    memset(&__DefaultRuneLocale, 0, sizeof(__DefaultRuneLocale));
    memcpy(__DefaultRuneLocale.__magic, "RuneMagi", 8);
    snprintf(__DefaultRuneLocale.__encoding, sizeof(__DefaultRuneLocale.__encoding), "NONE");
    __DefaultRuneLocale.__invalid_rune = -3;

    for (int c = 0; c < 256; ++c) {
        uint32_t f = 0;
        if (c < 128) {
            if (iscntrl(c))  f |= 0x00000200U; /* _CTYPE_C */
            if (isspace(c))  f |= 0x00004000U; /* _CTYPE_S */
            if (isblank(c))  f |= 0x00020000U; /* _CTYPE_B */
            if (isdigit(c))  f |= 0x00000400U | 0x00010000U | 0x00000800U | 0x00040000U; /* D|X|G|R */
            if (isupper(c))  f |= 0x00008000U | 0x00000100U | 0x00000800U | 0x00040000U; /* U|A|G|R */
            if (islower(c))  f |= 0x00001000U | 0x00000100U | 0x00000800U | 0x00040000U; /* L|A|G|R */
            if (isxdigit(c)) f |= 0x00010000U; /* _CTYPE_X */
            if (ispunct(c))  f |= 0x00002000U | 0x00000800U | 0x00040000U; /* P|G|R */
            if (isprint(c))  f |= 0x00040000U; /* _CTYPE_R */
        }
        __DefaultRuneLocale.__runetype[c] = f;
        __DefaultRuneLocale.__maplower[c] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
        __DefaultRuneLocale.__mapupper[c] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
    }
}

static void init_external_objc_classes(void) {
    /* 1. Initialize all 140 external ObjC classes + 140 external metaclasses */
#define INIT_EXT_CLASS_STRUCT(cls, sup) \
    do { \
        memset(&_OBJC_RO_META_$_##cls, 0, sizeof(DarwinClassRO)); \
        _OBJC_RO_META_$_##cls.flags         = 1U; /* RO_META */ \
        _OBJC_RO_META_$_##cls.instanceStart = 40U; \
        _OBJC_RO_META_$_##cls.instanceSize  = 40U; \
        _OBJC_RO_META_$_##cls.name          = #cls; \
        \
        memset(&_OBJC_RO_CLASS_$_##cls, 0, sizeof(DarwinClassRO)); \
        _OBJC_RO_CLASS_$_##cls.flags         = 0U; \
        _OBJC_RO_CLASS_$_##cls.instanceStart = 8U; \
        _OBJC_RO_CLASS_$_##cls.instanceSize  = 64U; \
        _OBJC_RO_CLASS_$_##cls.name          = #cls; \
        \
        _OBJC_METACLASS_$_##cls.isa                 = &_OBJC_METACLASS_$_NSObject; \
        _OBJC_METACLASS_$_##cls.superclass          = &_OBJC_METACLASS_$_##sup; \
        _OBJC_METACLASS_$_##cls.cache_buckets       = &__objc_empty_cache; \
        _OBJC_METACLASS_$_##cls.cache_mask_occupied = 0; \
        _OBJC_METACLASS_$_##cls.data_ptr            = (uint64_t)(uintptr_t)&_OBJC_RO_META_$_##cls; \
        \
        _OBJC_CLASS_$_##cls.isa                 = &_OBJC_METACLASS_$_##cls; \
        _OBJC_CLASS_$_##cls.superclass          = &_OBJC_CLASS_$_##sup; \
        _OBJC_CLASS_$_##cls.cache_buckets       = &__objc_empty_cache; \
        _OBJC_CLASS_$_##cls.cache_mask_occupied = 0; \
        _OBJC_CLASS_$_##cls.data_ptr            = (uint64_t)(uintptr_t)&_OBJC_RO_CLASS_$_##cls; \
        \
        g_class_by_name[#cls] = &_OBJC_CLASS_$_##cls; \
        g_symbol_table["_OBJC_CLASS_$_" #cls]     = &_OBJC_CLASS_$_##cls; \
        g_symbol_table["_OBJC_METACLASS_$_" #cls] = &_OBJC_METACLASS_$_##cls; \
    } while (0);

#define _OBJC_CLASS_$_NULL     _OBJC_CLASS_$_NSObject
#define _OBJC_METACLASS_$_NULL _OBJC_METACLASS_$_NSObject
    OH2_FOR_EACH_EXT_OBJC_CLASS(INIT_EXT_CLASS_STRUCT)
#undef _OBJC_CLASS_$_NULL
#undef _OBJC_METACLASS_$_NULL
#undef INIT_EXT_CLASS_STRUCT

    /* Fix root NSObject superclass & metaclass.superclass */
    _OBJC_CLASS_$_NSObject.superclass     = nullptr;
    _OBJC_METACLASS_$_NSObject.isa        = &_OBJC_METACLASS_$_NSObject;
    _OBJC_METACLASS_$_NSObject.superclass = &_OBJC_CLASS_$_NSObject;
    _OBJC_RO_META_$_NSObject.flags        = 3U; /* RO_META | RO_ROOT */
    _OBJC_RO_CLASS_$_NSObject.flags       = 2U; /* RO_ROOT */

    /* Configure ___CFConstantStringClassReference as a subclass of NSString */
    memset(&g_cfconst_str_meta_ro, 0, sizeof(DarwinClassRO));
    g_cfconst_str_meta_ro.flags = 1U;
    g_cfconst_str_meta_ro.instanceSize = 40U;
    g_cfconst_str_meta_ro.name = "__NSCFConstantString";
    g_cfconst_str_meta.isa = &_OBJC_METACLASS_$_NSObject;
    g_cfconst_str_meta.superclass = &_OBJC_METACLASS_$_NSString;
    g_cfconst_str_meta.cache_buckets = &__objc_empty_cache;
    g_cfconst_str_meta.data_ptr = (uint64_t)(uintptr_t)&g_cfconst_str_meta_ro;

    memset(&g_cfconst_str_cls_ro, 0, sizeof(DarwinClassRO));
    g_cfconst_str_cls_ro.instanceSize = 32U;
    g_cfconst_str_cls_ro.name = "__NSCFConstantString";
    ___CFConstantStringClassReference.isa = &g_cfconst_str_meta;
    ___CFConstantStringClassReference.superclass = &_OBJC_CLASS_$_NSString;
    ___CFConstantStringClassReference.cache_buckets = &__objc_empty_cache;
    ___CFConstantStringClassReference.data_ptr = (uint64_t)(uintptr_t)&g_cfconst_str_cls_ro;

    /* Configure __NSConcreteGlobalBlock & __NSConcreteStackBlock as subclasses of NSObject */
    memset(&g_global_block_ro, 0, sizeof(DarwinClassRO));
    g_global_block_ro.instanceSize = 32U;
    g_global_block_ro.name = "__NSGlobalBlock__";
    __NSConcreteGlobalBlock.isa = &_OBJC_METACLASS_$_NSObject;
    __NSConcreteGlobalBlock.superclass = &_OBJC_CLASS_$_NSObject;
    __NSConcreteGlobalBlock.cache_buckets = &__objc_empty_cache;
    __NSConcreteGlobalBlock.data_ptr = (uint64_t)(uintptr_t)&g_global_block_ro;

    memset(&g_stack_block_ro, 0, sizeof(DarwinClassRO));
    g_stack_block_ro.instanceSize = 32U;
    g_stack_block_ro.name = "__NSStackBlock__";
    __NSConcreteStackBlock.isa = &_OBJC_METACLASS_$_NSObject;
    __NSConcreteStackBlock.superclass = &_OBJC_CLASS_$_NSObject;
    __NSConcreteStackBlock.cache_buckets = &__objc_empty_cache;
    __NSConcreteStackBlock.data_ptr = (uint64_t)(uintptr_t)&g_stack_block_ro;

    /* Register core methods on NSObject */
    register_class_method(&_OBJC_CLASS_$_NSObject, "alloc", (void *)&imp_nsobject_alloc);
    register_class_method(&_OBJC_CLASS_$_NSObject, "allocWithZone:", (void *)&imp_nsobject_allocWithZone);
    register_class_method(&_OBJC_CLASS_$_NSObject, "new", (void *)&imp_nsobject_new);
    register_class_method(&_OBJC_CLASS_$_NSObject, "class", (void *)&imp_nsobject_class_meta);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "init", (void *)&imp_nsobject_init);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "self", (void *)&imp_nsobject_self);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "retain", (void *)&imp_nsobject_retain);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "release", (void *)&imp_nsobject_release);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "autorelease", (void *)&imp_nsobject_autorelease);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "dealloc", (void *)&imp_nsobject_dealloc);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "copy", (void *)&imp_nsobject_copy);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "mutableCopy", (void *)&imp_nsobject_copy);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "class", (void *)&imp_nsobject_class_inst);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "superclass", (void *)&imp_nsobject_superclass);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "respondsToSelector:", (void *)&imp_nsobject_respondsToSelector);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "isKindOfClass:", (void *)&imp_nsobject_isKindOfClass);
    register_instance_method(&_OBJC_CLASS_$_NSObject, "description", (void *)&imp_nsobject_description);

    /* Register NSString & NSMutableString methods */
    register_class_method(&_OBJC_CLASS_$_NSString, "string", (void *)&imp_nsstring_string);
    register_class_method(&_OBJC_CLASS_$_NSString, "stringWithUTF8String:", (void *)&imp_nsstring_stringWithUTF8String);
    register_class_method(&_OBJC_CLASS_$_NSString, "stringWithCString:encoding:", (void *)&imp_nsstring_stringWithCString_enc);
    register_class_method(&_OBJC_CLASS_$_NSString, "stringWithString:", (void *)&imp_nsstring_stringWithString);
    register_class_method(&_OBJC_CLASS_$_NSString, "stringWithFormat:", (void *)&darwin_objc_stringWithFormat);
    register_instance_method(&_OBJC_CLASS_$_NSString, "length", (void *)&imp_nsstring_length);
    register_instance_method(&_OBJC_CLASS_$_NSString, "UTF8String", (void *)&imp_nsstring_UTF8String);
    register_instance_method(&_OBJC_CLASS_$_NSString, "cStringUsingEncoding:", (void *)&imp_nsstring_UTF8String);
    register_instance_method(&_OBJC_CLASS_$_NSString, "initWithUTF8String:", (void *)&imp_nsstring_initWithUTF8String);
    register_instance_method(&_OBJC_CLASS_$_NSString, "initWithCString:encoding:", (void *)&imp_nsstring_initWithCString_enc);
    register_instance_method(&_OBJC_CLASS_$_NSString, "initWithString:", (void *)&imp_nsstring_initWithString);
    register_instance_method(&_OBJC_CLASS_$_NSString, "initWithFormat:", (void *)&darwin_objc_stringWithFormat);
    register_instance_method(&_OBJC_CLASS_$_NSString, "initWithContentsOfFile:", (void *)&imp_nsstring_initWithContentsOfFile);
    register_instance_method(&_OBJC_CLASS_$_NSString, "stringByAppendingString:", (void *)&imp_nsstring_stringByAppendingString);
    register_instance_method(&_OBJC_CLASS_$_NSString, "stringByAppendingPathComponent:", (void *)&imp_nsstring_stringByAppendingPathComponent);
    register_instance_method(&_OBJC_CLASS_$_NSString, "floatValue", (void *)&imp_nsstring_floatValue);
    register_instance_method(&_OBJC_CLASS_$_NSString, "doubleValue", (void *)&imp_nsstring_doubleValue);
    register_instance_method(&_OBJC_CLASS_$_NSString, "intValue", (void *)&imp_nsstring_intValue);
    register_instance_method(&_OBJC_CLASS_$_NSString, "integerValue", (void *)&imp_nsstring_intValue);
    register_instance_method(&_OBJC_CLASS_$_NSString, "isEqualToString:", (void *)&imp_nsstring_isEqualToString);
    register_instance_method(&_OBJC_CLASS_$_NSString, "isEqual:", (void *)&imp_nsstring_isEqualToString);
    register_instance_method(&_OBJC_CLASS_$_NSMutableString, "setString:", (void *)&imp_nsmutablestring_setString);
    register_instance_method(&_OBJC_CLASS_$_NSMutableString, "appendString:", (void *)&imp_nsmutablestring_appendString);
    register_instance_method(&_OBJC_CLASS_$_NSMutableString, "appendFormat:", (void *)&darwin_objc_appendFormat);

    /* Register NSBundle methods */
    register_class_method(&_OBJC_CLASS_$_NSBundle, "mainBundle", (void *)&imp_nsbundle_mainBundle);
    register_instance_method(&_OBJC_CLASS_$_NSBundle, "bundlePath", (void *)&imp_nsbundle_bundlePath);
    register_instance_method(&_OBJC_CLASS_$_NSBundle, "resourcePath", (void *)&imp_nsbundle_resourcePath);
    register_instance_method(&_OBJC_CLASS_$_NSBundle, "executablePath", (void *)&imp_nsbundle_executablePath);
    register_instance_method(&_OBJC_CLASS_$_NSBundle, "bundleIdentifier", (void *)&imp_nsbundle_bundleIdentifier);
    register_instance_method(&_OBJC_CLASS_$_NSBundle, "objectForInfoDictionaryKey:", (void *)&imp_nsbundle_objectForInfoDictionaryKey);
    register_instance_method(&_OBJC_CLASS_$_NSBundle, "infoDictionary", (void *)&imp_nsbundle_infoDictionary);

    /* Register UIDevice, UIScreen, UIApplication, UIView, UIWindow */
    register_class_method(&_OBJC_CLASS_$_UIDevice, "currentDevice", (void *)&imp_uidevice_currentDevice);
    register_instance_method(&_OBJC_CLASS_$_UIDevice, "systemVersion", (void *)&imp_uidevice_systemVersion);
    register_instance_method(&_OBJC_CLASS_$_UIDevice, "model", (void *)&imp_uidevice_model);
    register_instance_method(&_OBJC_CLASS_$_UIDevice, "batteryLevel", (void *)&imp_uidevice_batteryLevel);
    register_instance_method(&_OBJC_CLASS_$_UIDevice, "batteryState", (void *)&imp_uidevice_batteryState);

    register_class_method(&_OBJC_CLASS_$_UIScreen, "mainScreen", (void *)&imp_uiscreen_mainScreen);
    register_instance_method(&_OBJC_CLASS_$_UIScreen, "bounds", (void *)&imp_uiscreen_bounds);
    register_instance_method(&_OBJC_CLASS_$_UIScreen, "nativeBounds", (void *)&imp_uiscreen_bounds);
    register_instance_method(&_OBJC_CLASS_$_UIScreen, "scale", (void *)&imp_uiscreen_scale);
    register_instance_method(&_OBJC_CLASS_$_UIScreen, "nativeScale", (void *)&imp_uiscreen_scale);

    register_class_method(&_OBJC_CLASS_$_UIApplication, "sharedApplication", (void *)&imp_uiapplication_sharedApplication);
    register_instance_method(&_OBJC_CLASS_$_UIApplication, "statusBarOrientation", (void *)&imp_uiapplication_statusBarOrientation);
    register_instance_method(&_OBJC_CLASS_$_UIApplication, "delegate", (void *)&imp_uiapplication_delegate);
    register_instance_method(&_OBJC_CLASS_$_UIApplication, "setDelegate:", (void *)&imp_uiapplication_setDelegate);

    register_instance_method(&_OBJC_CLASS_$_UIView, "initWithFrame:", (void *)&imp_uiview_initWithFrame);
    register_instance_method(&_OBJC_CLASS_$_UIView, "frame", (void *)&imp_uiview_frame);
    register_instance_method(&_OBJC_CLASS_$_UIView, "bounds", (void *)&imp_uiview_frame);
    register_instance_method(&_OBJC_CLASS_$_UIView, "setFrame:", (void *)&imp_uiview_setFrame);
    register_instance_method(&_OBJC_CLASS_$_UIView, "layer", (void *)&imp_uiview_layer);

    /* Register NSProcessInfo, NSNotificationCenter, UNUserNotificationCenter, AVAudioSession, NSTimer, NSThread */
    register_class_method(&_OBJC_CLASS_$_NSProcessInfo, "processInfo", (void *)&imp_nsprocessinfo_processInfo);
    register_instance_method(&_OBJC_CLASS_$_NSProcessInfo, "thermalState", (void *)&imp_nsprocessinfo_thermalState);
    register_class_method(&_OBJC_CLASS_$_NSNotificationCenter, "defaultCenter", (void *)&imp_nsnotif_defaultCenter);
    register_class_method(&_OBJC_CLASS_$_UNUserNotificationCenter, "currentNotificationCenter", (void *)&imp_unuser_currentNotificationCenter);
    register_class_method(&_OBJC_CLASS_$_AVAudioSession, "sharedInstance", (void *)&imp_avaudiosession_sharedInstance);
    register_instance_method(&_OBJC_CLASS_$_AVAudioSession, "setCategory:error:", (void *)&imp_avaudiosession_true);
    register_instance_method(&_OBJC_CLASS_$_AVAudioSession, "setMode:error:", (void *)&imp_avaudiosession_true);
    register_instance_method(&_OBJC_CLASS_$_AVAudioSession, "setActive:error:", (void *)&imp_avaudiosession_true);
    register_instance_method(&_OBJC_CLASS_$_AVAudioSession, "outputVolume", (void *)&imp_avaudiosession_outputVolume);
    register_class_method(&_OBJC_CLASS_$_NSFileManager, "defaultManager", (void *)&imp_nsfilemanager_defaultManager);
    register_class_method(&_OBJC_CLASS_$_NSUserDefaults, "standardUserDefaults", (void *)&imp_nsuserdefaults_standardUserDefaults);
    register_class_method(&_OBJC_CLASS_$_NSTimer, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", (void *)&imp_nstimer_scheduledTimer);
    register_instance_method(&_OBJC_CLASS_$_NSThread, "initWithTarget:selector:object:", (void *)&imp_nsthread_initWithTarget);
    register_instance_method(&_OBJC_CLASS_$_NSThread, "start", (void *)&imp_nsthread_start);

    /* Register NSArray / NSMutableArray / NSDictionary / NSMutableDictionary */
    register_class_method(&_OBJC_CLASS_$_NSArray, "array", (void *)&imp_nsarray_array);
    register_instance_method(&_OBJC_CLASS_$_NSArray, "init", (void *)&imp_nsarray_init);
    register_instance_method(&_OBJC_CLASS_$_NSArray, "count", (void *)&imp_nsarray_count);
    register_instance_method(&_OBJC_CLASS_$_NSArray, "objectAtIndex:", (void *)&imp_nsarray_objectAtIndex);
    register_instance_method(&_OBJC_CLASS_$_NSMutableArray, "addObject:", (void *)&imp_nsmutablearray_addObject);

    register_class_method(&_OBJC_CLASS_$_NSDictionary, "dictionary", (void *)&imp_nsdict_dictionary);
    register_instance_method(&_OBJC_CLASS_$_NSDictionary, "objectForKey:", (void *)&imp_nsdict_objectForKey);
    register_instance_method(&_OBJC_CLASS_$_NSMutableDictionary, "setObject:forKey:", (void *)&imp_nsmutabledict_setObjectForKey);

    ___NSDictionary0__ = imp_nsdict_dictionary(&_OBJC_CLASS_$_NSDictionary, "dictionary");
}

} // namespace

extern "C" void oh2_symbol_resolver_init(MachOImage *img) {
    (void)img;
    if (!g_symbol_table.empty()) return;

    ___stdinp  = stdin;
    ___stdoutp = stdout;
    ___stderrp = stderr;

    init_rune_locale();

    /* Step 1: Seed all 542 functions with their diagnostic stub by default */
#define REGISTER_DEFAULT_STUB(fn_name) \
    g_symbol_table[#fn_name] = (void *)&stub_##fn_name;
    OH2_FOR_EACH_542_FUNCTION(REGISTER_DEFAULT_STUB)
#undef REGISTER_DEFAULT_STUB

    /* Step 2: Register all 140 External ObjC Classes + 140 Metaclasses */
    init_external_objc_classes();

    /* Step 3: Register Group-A (14) direct struct/array/Class-ISA globals */
    g_symbol_table["___CFConstantStringClassReference"] = (void *)&___CFConstantStringClassReference;
    g_symbol_table["__NSConcreteGlobalBlock"]           = (void *)&__NSConcreteGlobalBlock;
    g_symbol_table["__NSConcreteStackBlock"]            = (void *)&__NSConcreteStackBlock;
    g_symbol_table["__objc_empty_cache"]                = (void *)&__objc_empty_cache;
    g_symbol_table["__DefaultRuneLocale"]               = (void *)&__DefaultRuneLocale;
    g_symbol_table["__dispatch_main_q"]                 = (void *)&__dispatch_main_q;
    g_symbol_table["_NDR_record"]                       = (void *)&_NDR_record;
    g_symbol_table["__FE_DFL_ENV"]                      = (void *)&__FE_DFL_ENV;
    g_symbol_table["_in6addr_any"]                      = (void *)&_in6addr_any;
    g_symbol_table["_in6addr_loopback"]                 = (void *)&_in6addr_loopback;
    g_symbol_table["_tzname"]                           = (void *)&_tzname;
    g_symbol_table["_kCFTypeArrayCallBacks"]            = (void *)&_kCFTypeArrayCallBacks;
    g_symbol_table["_kCMTimeZero"]                      = (void *)&_kCMTimeZero;
    g_symbol_table["_kCMTimePositiveInfinity"]          = (void *)&_kCMTimePositiveInfinity;

    /* Step 4: Register Group-B (98) single-dereference scalar/pointer globals */
    g_symbol_table["___stack_chk_guard"]              = (void *)&___stack_chk_guard;
    g_symbol_table["___stdinp"]                       = (void *)&___stdinp;
    g_symbol_table["___stdoutp"]                      = (void *)&___stdoutp;
    g_symbol_table["___stderrp"]                      = (void *)&___stderrp;
    g_symbol_table["_mach_task_self_"]                = (void *)&_mach_task_self_;
    g_symbol_table["_vm_page_size"]                   = (void *)&_vm_page_size;
    g_symbol_table["_vm_page_mask"]                   = (void *)&_vm_page_mask;
    g_symbol_table["___NSDictionary0__"]              = (void *)&___NSDictionary0__;
    g_symbol_table["_kCFAllocatorDefault"]            = (void *)&_kCFAllocatorDefault;
    g_symbol_table["_kCFAllocatorSystemDefault"]      = (void *)&_kCFAllocatorSystemDefault;
    g_symbol_table["_kCFAllocatorNull"]               = (void *)&kCFAllocatorNull;
    g_symbol_table["kCFAllocatorNull"]                = (void *)&kCFAllocatorNull;
    g_symbol_table["_kCIFormatARGB8"]                 = (void *)&_kCIFormatARGB8;
    g_symbol_table["_UIAccessibilityTraitNone"]       = (void *)&_UIAccessibilityTraitNone;
    g_symbol_table["_UIAccessibilityTraitButton"]     = (void *)&_UIAccessibilityTraitButton;
    g_symbol_table["_UIAccessibilityTraitLink"]       = (void *)&_UIAccessibilityTraitLink;
    g_symbol_table["_UIAccessibilityTraitImage"]      = (void *)&_UIAccessibilityTraitImage;
    g_symbol_table["_UIAccessibilityTraitNotEnabled"] = (void *)&_UIAccessibilityTraitNotEnabled;
    g_symbol_table["_UIAccessibilityTraitAdjustable"] = (void *)&_UIAccessibilityTraitAdjustable;
    g_symbol_table["_CKQueryOperationMaximumResults"] = (void *)&_CKQueryOperationMaximumResults;

#define REGISTER_GROUP_B_CFSTRING(sym, lit) \
    g_symbol_table[#sym] = (void *)&sym;
    OH2_FOR_EACH_GROUP_B_CFSTRING(REGISTER_GROUP_B_CFSTRING)
#undef REGISTER_GROUP_B_CFSTRING

    /* Step 5: Register all 16 libc++.1.dylib (std::__1 + __cxxabiv1) symbols */
    g_symbol_table["__ZNSt3__112__next_primeEm"]         = (void *)static_cast<size_t (*)(size_t)>(&std::__ndk1::__next_prime);
    g_symbol_table["__ZNSt3__15mutex4lockEv"]            = (void *)&darwin_std_mutex_lock;
    g_symbol_table["__ZNSt3__15mutex6unlockEv"]          = (void *)&darwin_std_mutex_unlock;
    g_symbol_table["__ZSt9terminatev"]                   = (void *)&std::terminate;
    g_symbol_table["__ZTVN10__cxxabiv117__class_type_infoE"]    = (void *)&_ZTVN10__cxxabiv117__class_type_infoE;
    g_symbol_table["__ZTVN10__cxxabiv120__si_class_type_infoE"] = (void *)&_ZTVN10__cxxabiv120__si_class_type_infoE;
    g_symbol_table["__ZTVN10__cxxabiv121__vmi_class_type_infoE"]= (void *)&_ZTVN10__cxxabiv121__vmi_class_type_infoE;
    g_symbol_table["___cxa_bad_typeid"]                  = (void *)&__cxxabiv1::__cxa_bad_typeid;
    g_symbol_table["___cxa_begin_catch"]                 = (void *)&__cxxabiv1::__cxa_begin_catch;
    g_symbol_table["___cxa_demangle"]                    = (void *)&__cxxabiv1::__cxa_demangle;
    g_symbol_table["___cxa_guard_abort"]                 = (void *)&__cxxabiv1::__cxa_guard_abort;
    g_symbol_table["___cxa_guard_acquire"]               = (void *)&__cxxabiv1::__cxa_guard_acquire;
    g_symbol_table["___cxa_guard_release"]               = (void *)&__cxxabiv1::__cxa_guard_release;
    g_symbol_table["___cxa_pure_virtual"]                = (void *)&__cxxabiv1::__cxa_pure_virtual;
    g_symbol_table["___dynamic_cast"]                    = (void *)&__dynamic_cast;
    g_symbol_table["___gxx_personality_v0"]              = (void *)&__gxx_personality_v0;

    /* Step 6: Register all 12 variadic + 2 va_list trampolines */
    g_symbol_table["_printf"]         = (void *)&darwin_printf;
    g_symbol_table["_fprintf"]        = (void *)&darwin_fprintf;
    g_symbol_table["_sprintf"]        = (void *)&darwin_sprintf;
    g_symbol_table["_snprintf"]       = (void *)&darwin_snprintf;
    g_symbol_table["___sprintf_chk"]  = (void *)&darwin_sprintf_chk;
    g_symbol_table["___snprintf_chk"] = (void *)&darwin_snprintf_chk;
    g_symbol_table["_sscanf"]         = (void *)&darwin_sscanf;
    g_symbol_table["_NSLog"]          = (void *)&darwin_NSLog;
    g_symbol_table["_open"]           = (void *)&darwin_open;
    g_symbol_table["_fcntl"]          = (void *)&darwin_fcntl;
    g_symbol_table["_ioctl"]          = (void *)&darwin_ioctl;
    g_symbol_table["_syscall"]        = (void *)&darwin_syscall;
    g_symbol_table["_vfprintf"]       = (void *)&darwin_vfprintf;
    g_symbol_table["_vsnprintf"]      = (void *)&darwin_vsnprintf;

    /* Step 7: Register libz.1.dylib (12 symbols) */
    g_symbol_table["_compress2"]     = (void *)&compress2;
    g_symbol_table["_compressBound"] = (void *)&compressBound;
    g_symbol_table["_crc32"]         = (void *)&crc32;
    g_symbol_table["_deflate"]       = (void *)&deflate;
    g_symbol_table["_deflateEnd"]    = (void *)&deflateEnd;
    g_symbol_table["_deflateInit2_"] = (void *)&deflateInit2_;
    g_symbol_table["_deflateReset"]  = (void *)&deflateReset;
    g_symbol_table["_inflate"]       = (void *)&inflate;
    g_symbol_table["_inflateEnd"]    = (void *)&inflateEnd;
    g_symbol_table["_inflateInit2_"] = (void *)&inflateInit2_;
    g_symbol_table["_inflateInit_"]  = (void *)&inflateInit_;
    g_symbol_table["_inflateReset"]  = (void *)&inflateReset;

    /* Step 8: Register libobjc.A.dylib (26 functions) */
    g_symbol_table["_class_getInstanceSize"]         = (void *)&darwin_class_getInstanceSize;
    g_symbol_table["_class_getMethodImplementation"] = (void *)&darwin_class_getMethodImplementation;
    g_symbol_table["_objc_alloc"]                    = (void *)&darwin_objc_alloc;
    g_symbol_table["_objc_autorelease"]              = (void *)&darwin_objc_autorelease;
    g_symbol_table["_objc_autoreleasePoolPop"]       = (void *)&darwin_objc_autoreleasePoolPop;
    g_symbol_table["_objc_autoreleasePoolPush"]      = (void *)&darwin_objc_autoreleasePoolPush;
    g_symbol_table["_objc_constructInstance"]        = (void *)&darwin_objc_constructInstance;
    g_symbol_table["_objc_copyCppObjectAtomic"]      = (void *)&darwin_objc_copyCppObjectAtomic;
    g_symbol_table["_objc_destructInstance"]         = (void *)&darwin_objc_destructInstance;
    g_symbol_table["_objc_enumerationMutation"]      = (void *)&darwin_objc_enumerationMutation;
    g_symbol_table["_objc_getAssociatedObject"]      = (void *)&darwin_objc_getAssociatedObject;
    g_symbol_table["_objc_getProperty"]              = (void *)&darwin_objc_getProperty;
    g_symbol_table["_objc_getRequiredClass"]         = (void *)&darwin_objc_getRequiredClass;
    g_symbol_table["_objc_msgSend"]                  = (void *)&darwin_objc_msgSend;
    g_symbol_table["_objc_msgSendSuper2"]            = (void *)&darwin_objc_msgSendSuper2;
    g_symbol_table["_objc_release"]                  = (void *)&darwin_objc_release;
    g_symbol_table["_objc_retain"]                   = (void *)&darwin_objc_retain;
    g_symbol_table["_objc_setAssociatedObject"]      = (void *)&darwin_objc_setAssociatedObject;
    g_symbol_table["_objc_setProperty_atomic"]       = (void *)&darwin_objc_setProperty_atomic;
    g_symbol_table["_objc_setProperty_atomic_copy"]  = (void *)&darwin_objc_setProperty_atomic_copy;
    g_symbol_table["_objc_setProperty_nonatomic"]    = (void *)&darwin_objc_setProperty_nonatomic;
    g_symbol_table["_objc_setProperty_nonatomic_copy"]= (void *)&darwin_objc_setProperty_nonatomic_copy;
    g_symbol_table["_objc_sync_enter"]               = (void *)&darwin_objc_sync_enter;
    g_symbol_table["_objc_sync_exit"]                = (void *)&darwin_objc_sync_exit;
    g_symbol_table["_object_getClass"]               = (void *)&darwin_object_getClass;
    g_symbol_table["_object_setClass"]               = (void *)&darwin_object_setClass;

    /* Step 9: Register CoreFoundation, Foundation, UIKit & QuartzCore functions */
    g_symbol_table["_CFArrayCreate"]                  = (void *)&darwin_CFArrayCreate;
    g_symbol_table["_CFArrayGetCount"]                = (void *)&darwin_CFArrayGetCount;
    g_symbol_table["_CFArrayGetValueAtIndex"]         = (void *)&darwin_CFArrayGetValueAtIndex;
    g_symbol_table["_CFBooleanGetValue"]              = (void *)&darwin_CFBooleanGetValue;
    g_symbol_table["_CFDataCreateWithBytesNoCopy"]    = (void *)&darwin_CFDataCreateWithBytesNoCopy;
    g_symbol_table["_CFDataGetBytePtr"]               = (void *)&darwin_CFDataGetBytePtr;
    g_symbol_table["_CFDataGetLength"]                = (void *)&darwin_CFDataGetLength;
    g_symbol_table["_CFDictionaryGetValue"]           = (void *)&darwin_CFDictionaryGetValue;
    g_symbol_table["_CFLocaleCopyCurrent"]            = (void *)&darwin_CFLocaleCopyCurrent;
    g_symbol_table["_CFLocaleCopyPreferredLanguages"] = (void *)&darwin_CFLocaleCopyPreferredLanguages;
    g_symbol_table["_CFLocaleGetValue"]               = (void *)&darwin_CFLocaleGetValue;
    g_symbol_table["_CFRelease"]                      = (void *)&darwin_CFRelease;
    g_symbol_table["_CFRetain"]                       = (void *)&darwin_CFRetain;
    g_symbol_table["_CFStringCreateWithBytes"]        = (void *)&darwin_CFStringCreateWithBytes;
    g_symbol_table["_CFStringCreateWithCStringNoCopy"]= (void *)&darwin_CFStringCreateWithCStringNoCopy;
    g_symbol_table["_CFStringGetBytes"]               = (void *)&darwin_CFStringGetBytes;
    g_symbol_table["_CFStringGetCString"]             = (void *)&darwin_CFStringGetCString;
    g_symbol_table["_CFStringGetLength"]              = (void *)&darwin_CFStringGetLength;
    g_symbol_table["_CFGetTypeID"]                    = (void *)&darwin_CFGetTypeID;
    g_symbol_table["_CFStringGetTypeID"]              = (void *)&darwin_CFStringGetTypeID;
    g_symbol_table["_CFPropertyListCreateWithData"]   = (void *)&darwin_CFPropertyListCreateWithData;
    g_symbol_table["_CFPropertyListCreateFromXMLData"]= (void *)&darwin_CFPropertyListCreateWithData;
    g_symbol_table["_CFUUIDCreate"]                   = (void *)&darwin_CFUUIDCreate;
    g_symbol_table["_CFUUIDCreateFromUUIDBytes"]      = (void *)&darwin_CFUUIDCreateFromUUIDBytes;
    g_symbol_table["_CFUUIDCreateString"]             = (void *)&darwin_CFUUIDCreateString;
    g_symbol_table["_CFUUIDGetUUIDBytes"]             = (void *)&darwin_CFUUIDGetUUIDBytes;

    g_symbol_table["_NSHomeDirectory"]                       = (void *)&darwin_NSHomeDirectory;
    g_symbol_table["_NSSearchPathForDirectoriesInDomains"]   = (void *)&darwin_NSSearchPathForDirectoriesInDomains;
    g_symbol_table["_NSSetUncaughtExceptionHandler"]         = (void *)&darwin_NSSetUncaughtExceptionHandler;
    g_symbol_table["_NSStringFromClass"]                     = (void *)&darwin_NSStringFromClass;
    g_symbol_table["_NSStringFromSelector"]                  = (void *)&darwin_NSStringFromSelector;
    g_symbol_table["_NSTemporaryDirectory"]                  = (void *)&darwin_NSTemporaryDirectory;
    g_symbol_table["_UIApplicationMain"]                     = (void *)&darwin_UIApplicationMain;
    g_symbol_table["_CACurrentMediaTime"]                    = (void *)&darwin_CACurrentMediaTime;

    /* Step 10: Register libSystem.B.dylib POSIX, Math, Memory, Pthread & Mach functions */
    g_symbol_table["dyld_stub_binder"]                 = (void *)&darwin_dyld_stub_binder;
    g_symbol_table["__availability_version_check"]     = (void *)&darwin_availability_version_check;
    g_symbol_table["_availability_version_check"]      = (void *)&darwin_availability_version_check;
    g_symbol_table["__Unwind_Resume"]                  = (void *)&_Unwind_Resume;
    g_symbol_table["___cxa_atexit"]                    = (void *)&darwin_cxa_atexit;
    g_symbol_table["___error"]                         = (void *)&darwin_error;
    g_symbol_table["___maskrune"]                      = (void *)&darwin_maskrune;
    g_symbol_table["___tolower"]                       = (void *)&darwin_tolower;
    g_symbol_table["___exp10"]                         = (void *)&darwin_exp10;
    g_symbol_table["___exp10f"]                        = (void *)&darwin_exp10f;
    g_symbol_table["___isfinitef"]                     = (void *)&darwin_isfinitef;
    g_symbol_table["___sincos_stret"]                  = (void *)&darwin_sincos_stret;
    g_symbol_table["___sincosf_stret"]                 = (void *)&darwin_sincosf_stret;
    g_symbol_table["___memcpy_chk"]                    = (void *)&darwin_memcpy_chk;
    g_symbol_table["___memmove_chk"]                   = (void *)&darwin_memmove_chk;
    g_symbol_table["___memset_chk"]                    = (void *)&darwin_memset_chk;
    g_symbol_table["___strcat_chk"]                    = (void *)&darwin_strcat_chk;
    g_symbol_table["___strncat_chk"]                   = (void *)&darwin_strncat_chk;
    g_symbol_table["___darwin_check_fd_set_overflow"]  = (void *)&darwin_check_fd_set_overflow;
    g_symbol_table["___assert_rtn"]                    = (void *)&darwin_assert_rtn;
    g_symbol_table["___stack_chk_fail"]                = (void *)&darwin_stack_chk_fail;
    g_symbol_table["__NSGetExecutablePath"]            = (void *)&darwin_NSGetExecutablePath;
    g_symbol_table["__Exit"]                           = (void *)&_Exit;

    g_symbol_table["__Block_copy"]                     = (void *)&darwin_Block_copy;
    g_symbol_table["__Block_release"]                  = (void *)&darwin_Block_release;
    g_symbol_table["__Block_object_assign"]            = (void *)&darwin_Block_object_assign;
    g_symbol_table["__Block_object_dispose"]           = (void *)&darwin_Block_object_dispose;

    g_symbol_table["_dispatch_async"]                  = (void *)&darwin_dispatch_async;
    g_symbol_table["_dispatch_get_global_queue"]       = (void *)&darwin_dispatch_get_global_queue;
    g_symbol_table["_dispatch_once_f"]                 = (void *)&darwin_dispatch_once_f;
    g_symbol_table["_dispatch_release"]                = (void *)&darwin_dispatch_release;
    g_symbol_table["_dispatch_retain"]                 = (void *)&darwin_dispatch_retain;
    g_symbol_table["_dispatch_semaphore_create"]       = (void *)&darwin_dispatch_semaphore_create;
    g_symbol_table["_dispatch_semaphore_signal"]       = (void *)&darwin_dispatch_semaphore_signal;
    g_symbol_table["_dispatch_semaphore_wait"]         = (void *)&darwin_dispatch_semaphore_wait;

    g_symbol_table["_OSAtomicAdd32Barrier"]            = (void *)&darwin_OSAtomicAdd32Barrier;
    g_symbol_table["_OSAtomicCompareAndSwap32Barrier"] = (void *)&darwin_OSAtomicCompareAndSwap32Barrier;
    g_symbol_table["_OSAtomicCompareAndSwap64Barrier"] = (void *)&darwin_OSAtomicCompareAndSwap64Barrier;
    g_symbol_table["_OSAtomicCompareAndSwapPtrBarrier"]= (void *)&darwin_OSAtomicCompareAndSwapPtrBarrier;
    g_symbol_table["_OSMemoryBarrier"]                 = (void *)&darwin_OSMemoryBarrier;
    g_symbol_table["_OSSpinLockLock"]                  = (void *)&darwin_OSSpinLockLock;
    g_symbol_table["_OSSpinLockUnlock"]                = (void *)&darwin_OSSpinLockUnlock;
    g_symbol_table["_CC_SHA256"]                       = (void *)&darwin_CC_SHA256;
    g_symbol_table["_uuid_generate"]                   = (void *)&darwin_uuid_generate;

    g_symbol_table["_dladdr"]                          = (void *)&darwin_dladdr;
    g_symbol_table["_dlclose"]                         = (void *)&darwin_dlclose;
    g_symbol_table["_dlerror"]                         = (void *)&darwin_dlerror;
    g_symbol_table["_dlopen"]                          = (void *)&darwin_dlopen;
    g_symbol_table["_dlsym"]                           = (void *)&darwin_dlsym;

    g_symbol_table["_mach_absolute_time"]              = (void *)&darwin_mach_absolute_time;
    g_symbol_table["_mach_timebase_info"]              = (void *)&darwin_mach_timebase_info;
    g_symbol_table["_mach_host_self"]                  = (void *)&darwin_pthread_mach_thread_np;
    g_symbol_table["_mach_thread_self"]                = (void *)&darwin_pthread_mach_thread_np;
    g_symbol_table["_host_page_size"]                  = (void *)&darwin_host_page_size;
    g_symbol_table["_host_statistics"]                 = (void *)&darwin_host_statistics;
    g_symbol_table["_task_info"]                       = (void *)&darwin_task_info;
    g_symbol_table["_vm_allocate"]                     = (void *)&darwin_vm_allocate;
    g_symbol_table["_vm_deallocate"]                   = (void *)&darwin_vm_deallocate;
    g_symbol_table["_vm_protect"]                      = (void *)&darwin_vm_protect;

    g_symbol_table["_pthread_attr_destroy"]            = (void *)&pthread_attr_destroy;
    g_symbol_table["_pthread_attr_init"]               = (void *)&pthread_attr_init;
    g_symbol_table["_pthread_attr_setdetachstate"]     = (void *)&pthread_attr_setdetachstate;
    g_symbol_table["_pthread_attr_setstacksize"]       = (void *)&pthread_attr_setstacksize;
    g_symbol_table["_pthread_cancel"]                  = (void *)&darwin_pthread_cancel;
    g_symbol_table["_pthread_cond_broadcast"]          = (void *)&darwin_pthread_cond_broadcast;
    g_symbol_table["_pthread_cond_destroy"]            = (void *)&darwin_pthread_cond_destroy;
    g_symbol_table["_pthread_cond_init"]               = (void *)&darwin_pthread_cond_init;
    g_symbol_table["_pthread_cond_signal"]             = (void *)&darwin_pthread_cond_signal;
    g_symbol_table["_pthread_cond_timedwait"]          = (void *)&darwin_pthread_cond_timedwait;
    g_symbol_table["_pthread_cond_wait"]               = (void *)&darwin_pthread_cond_wait;
    g_symbol_table["_pthread_create"]                  = (void *)&pthread_create;
    g_symbol_table["_pthread_exit"]                    = (void *)&pthread_exit;
    g_symbol_table["_pthread_from_mach_thread_np"]     = (void *)&darwin_pthread_from_mach_thread_np;
    g_symbol_table["_pthread_getschedparam"]           = (void *)&pthread_getschedparam;
    g_symbol_table["_pthread_getspecific"]             = (void *)&darwin_pthread_getspecific;
    g_symbol_table["_pthread_join"]                    = (void *)&pthread_join;
    g_symbol_table["_pthread_key_create"]              = (void *)&darwin_pthread_key_create;
    g_symbol_table["_pthread_key_delete"]              = (void *)&darwin_pthread_key_delete;
    g_symbol_table["_pthread_kill"]                    = (void *)&pthread_kill;
    g_symbol_table["_pthread_mach_thread_np"]          = (void *)&darwin_pthread_mach_thread_np;
    g_symbol_table["_pthread_mutex_destroy"]           = (void *)&darwin_pthread_mutex_destroy;
    g_symbol_table["_pthread_mutex_init"]              = (void *)&darwin_pthread_mutex_init;
    g_symbol_table["_pthread_mutex_lock"]              = (void *)&darwin_pthread_mutex_lock;
    g_symbol_table["_pthread_mutex_trylock"]           = (void *)&darwin_pthread_mutex_trylock;
    g_symbol_table["_pthread_mutex_unlock"]            = (void *)&darwin_pthread_mutex_unlock;
    g_symbol_table["_pthread_mutexattr_destroy"]       = (void *)&darwin_pthread_mutexattr_destroy;
    g_symbol_table["_pthread_mutexattr_init"]          = (void *)&darwin_pthread_mutexattr_init;
    g_symbol_table["_pthread_mutexattr_setprotocol"]   = (void *)&darwin_pthread_mutexattr_setprotocol;
    g_symbol_table["_pthread_mutexattr_settype"]       = (void *)&darwin_pthread_mutexattr_settype;
    g_symbol_table["_pthread_rwlock_destroy"]          = (void *)&pthread_rwlock_destroy;
    g_symbol_table["_pthread_rwlock_init"]             = (void *)&pthread_rwlock_init;
    g_symbol_table["_pthread_rwlock_rdlock"]           = (void *)&pthread_rwlock_rdlock;
    g_symbol_table["_pthread_rwlock_unlock"]           = (void *)&pthread_rwlock_unlock;
    g_symbol_table["_pthread_rwlock_wrlock"]           = (void *)&pthread_rwlock_wrlock;
    g_symbol_table["_pthread_self"]                    = (void *)&pthread_self;
    g_symbol_table["_pthread_setname_np"]              = (void *)&darwin_pthread_setname_np;
    g_symbol_table["_pthread_setschedparam"]           = (void *)&pthread_setschedparam;
    g_symbol_table["_pthread_setspecific"]             = (void *)&darwin_pthread_setspecific;

    g_symbol_table["_abort"]       = (void *)&abort;
    g_symbol_table["_accept"]      = (void *)&accept;
    g_symbol_table["_access"]      = (void *)&access;
    g_symbol_table["_acos"]        = (void *)static_cast<double(*)(double)>(&acos);
    g_symbol_table["_acosf"]       = (void *)&acosf;
    g_symbol_table["_asin"]        = (void *)static_cast<double(*)(double)>(&asin);
    g_symbol_table["_asinf"]       = (void *)&asinf;
    g_symbol_table["_atan"]        = (void *)static_cast<double(*)(double)>(&atan);
    g_symbol_table["_atan2"]       = (void *)static_cast<double(*)(double,double)>(&atan2);
    g_symbol_table["_atan2f"]      = (void *)&atan2f;
    g_symbol_table["_atanf"]       = (void *)&atanf;
    g_symbol_table["_atof"]        = (void *)&atof;
    g_symbol_table["_atoi"]        = (void *)&atoi;
    g_symbol_table["_atol"]        = (void *)&atol;
    g_symbol_table["_bind"]        = (void *)&bind;
    g_symbol_table["_bsearch"]     = (void *)static_cast<void*(*)(const void*,const void*,size_t,size_t,int(*)(const void*,const void*))>(&bsearch);
    g_symbol_table["_bzero"]       = (void *)&darwin_bzero;
    g_symbol_table["_calloc"]      = (void *)&calloc;
    g_symbol_table["_chmod"]       = (void *)&chmod;
    g_symbol_table["_close"]       = (void *)&close;
    g_symbol_table["_closedir"]    = (void *)&closedir;
    g_symbol_table["_connect"]     = (void *)&connect;
    g_symbol_table["_cos"]         = (void *)static_cast<double(*)(double)>(&cos);
    g_symbol_table["_cosf"]        = (void *)&cosf;
    g_symbol_table["_exit"]        = (void *)&exit;
    g_symbol_table["_exp"]         = (void *)static_cast<double(*)(double)>(&exp);
    g_symbol_table["_exp2"]        = (void *)static_cast<double(*)(double)>(&exp2);
    g_symbol_table["_exp2f"]       = (void *)&exp2f;
    g_symbol_table["_expf"]        = (void *)&expf;
    g_symbol_table["_fclose"]      = (void *)&fclose;
    g_symbol_table["_fegetenv"]    = (void *)&fegetenv;
    g_symbol_table["_feof"]        = (void *)&feof;
    g_symbol_table["_ferror"]      = (void *)&ferror;
    g_symbol_table["_fesetenv"]    = (void *)&fesetenv;
    g_symbol_table["_fflush"]      = (void *)&fflush;
    g_symbol_table["_fgets"]       = (void *)&fgets;
    g_symbol_table["_fileno"]      = (void *)&fileno;
    g_symbol_table["_fmod"]        = (void *)static_cast<double(*)(double,double)>(&fmod);
    g_symbol_table["_fmodf"]       = (void *)&fmodf;
    g_symbol_table["_fopen"]       = (void *)&fopen;
    g_symbol_table["_fputc"]       = (void *)&darwin_fputc;
    g_symbol_table["_fputs"]       = (void *)&darwin_fputs;
    g_symbol_table["_fread"]       = (void *)&fread;
    g_symbol_table["_free"]        = (void *)&free;
    g_symbol_table["_freeaddrinfo"]= (void *)&freeaddrinfo;
    g_symbol_table["_frexpf"]      = (void *)static_cast<float(*)(float,int*)>(&frexpf);
    g_symbol_table["_fseek"]       = (void *)&fseek;
    g_symbol_table["_fstat"]       = (void *)&darwin_fstat;
    g_symbol_table["_fsync"]       = (void *)&fsync;
    g_symbol_table["_ftell"]       = (void *)&ftell;
    g_symbol_table["_ftruncate"]   = (void *)&ftruncate;
    g_symbol_table["_fwrite"]      = (void *)&fwrite;
    g_symbol_table["_getaddrinfo"] = (void *)&getaddrinfo;
    g_symbol_table["_getenv"]      = (void *)&getenv;
    g_symbol_table["_gethostbyaddr"]= (void *)&gethostbyaddr;
    g_symbol_table["_gethostbyname"]= (void *)&gethostbyname;
    g_symbol_table["_gethostname"] = (void *)&gethostname;
    g_symbol_table["_getnameinfo"] = (void *)&getnameinfo;
    g_symbol_table["_getpeername"] = (void *)&getpeername;
    g_symbol_table["_getpid"]      = (void *)&getpid;
    g_symbol_table["_getprogname"] = (void *)&getprogname;
    g_symbol_table["_getrusage"]   = (void *)&getrusage;
    g_symbol_table["_getsockname"] = (void *)&getsockname;
    g_symbol_table["_getsockopt"]  = (void *)&getsockopt;
    g_symbol_table["_gettimeofday"]= (void *)&gettimeofday;
    g_symbol_table["_getuid"]      = (void *)&getuid;
    g_symbol_table["_gmtime"]      = (void *)&gmtime;
    g_symbol_table["_gmtime_r"]    = (void *)&gmtime_r;
    g_symbol_table["_if_nametoindex"]= (void *)&if_nametoindex;
    g_symbol_table["_inet_addr"]   = (void *)&inet_addr;
    g_symbol_table["_inet_pton"]   = (void *)&inet_pton;
    g_symbol_table["_ldexp"]       = (void *)static_cast<double(*)(double,int)>(&ldexp);
    g_symbol_table["_ldexpf"]      = (void *)static_cast<float(*)(float,int)>(&ldexpf);
    g_symbol_table["_listen"]      = (void *)&listen;
    g_symbol_table["_localtime"]   = (void *)&localtime;
    g_symbol_table["_localtime_r"] = (void *)&localtime_r;
    g_symbol_table["_log"]         = (void *)static_cast<double(*)(double)>(&log);
    g_symbol_table["_log10"]       = (void *)static_cast<double(*)(double)>(&log10);
    g_symbol_table["_log10f"]      = (void *)&log10f;
    g_symbol_table["_logf"]        = (void *)&logf;
    g_symbol_table["_longjmp"]     = (void *)&longjmp;
    g_symbol_table["_lseek"]       = (void *)&lseek;
    g_symbol_table["_malloc"]      = (void *)&malloc;
    g_symbol_table["_malloc_size"] = (void *)&darwin_malloc_size;
    g_symbol_table["_memchr"]      = (void *)&darwin_memchr;
    g_symbol_table["_memcmp"]      = (void *)&memcmp;
    g_symbol_table["_memcpy"]      = (void *)&memcpy;
    g_symbol_table["_memmove"]     = (void *)&memmove;
    g_symbol_table["_memset"]      = (void *)&memset;
    g_symbol_table["_memset_pattern4"]  = (void *)&darwin_memset_pattern4;
    g_symbol_table["_memset_pattern8"]  = (void *)&darwin_memset_pattern8;
    g_symbol_table["_memset_pattern16"] = (void *)&darwin_memset_pattern16;
    g_symbol_table["_mkstemp"]     = (void *)&mkstemp;
    g_symbol_table["_mktime"]      = (void *)&mktime;
    g_symbol_table["_mmap"]        = (void *)&darwin_mmap;
    g_symbol_table["_modf"]        = (void *)static_cast<double(*)(double,double*)>(&modf);
    g_symbol_table["_modff"]       = (void *)static_cast<float(*)(float,float*)>(&modff);
    g_symbol_table["_munmap"]      = (void *)&munmap;
    g_symbol_table["_opendir"]     = (void *)&opendir;
    g_symbol_table["_pause"]       = (void *)&pause;
    g_symbol_table["_posix_madvise"]= (void *)&darwin_posix_madvise;
    g_symbol_table["_pow"]         = (void *)static_cast<double(*)(double,double)>(&pow);
    g_symbol_table["_powf"]        = (void *)&powf;
    g_symbol_table["_putchar"]     = (void *)&darwin_putchar;
    g_symbol_table["_puts"]        = (void *)&darwin_puts;
    g_symbol_table["_qsort"]       = (void *)&qsort;
    g_symbol_table["_raise"]       = (void *)&raise;
    g_symbol_table["_rand"]        = (void *)&rand;
    g_symbol_table["_read"]        = (void *)&read;
    g_symbol_table["_readdir"]     = (void *)&readdir;
    g_symbol_table["_realloc"]     = (void *)&realloc;
    g_symbol_table["_recv"]        = (void *)&recv;
    g_symbol_table["_recvfrom"]    = (void *)&recvfrom;
    g_symbol_table["_rename"]      = (void *)&rename;
    g_symbol_table["_rewind"]      = (void *)&rewind;
    g_symbol_table["_rmdir"]       = (void *)&rmdir;
    g_symbol_table["_sched_yield"] = (void *)&sched_yield;
    g_symbol_table["_select"]      = (void *)&select;
    g_symbol_table["_send"]        = (void *)&send;
    g_symbol_table["_sendto"]      = (void *)&sendto;
    g_symbol_table["_setjmp"]      = (void *)&setjmp;
    g_symbol_table["_setlocale"]   = (void *)&setlocale;
    g_symbol_table["_setrlimit"]   = (void *)&setrlimit;
    g_symbol_table["_setsockopt"]  = (void *)&setsockopt;
    g_symbol_table["_setvbuf"]     = (void *)&setvbuf;
    g_symbol_table["_shutdown"]    = (void *)&shutdown;
    g_symbol_table["_sigaction"]   = (void *)&sigaction;
    g_symbol_table["_sigaltstack"] = (void *)&sigaltstack;
    g_symbol_table["_signal"]      = (void *)&signal;
    g_symbol_table["_sin"]         = (void *)static_cast<double(*)(double)>(&sin);
    g_symbol_table["_sinf"]        = (void *)&sinf;
    g_symbol_table["_sinhf"]       = (void *)&sinhf;
    g_symbol_table["_socket"]      = (void *)&socket;
    g_symbol_table["_srand"]       = (void *)&srand;
    g_symbol_table["_stat"]        = (void *)&darwin_stat;
    g_symbol_table["_strcasecmp"]  = (void *)&strcasecmp;
    g_symbol_table["_strcat"]      = (void *)&strcat;
    g_symbol_table["_strchr"]      = (void *)&darwin_strchr;
    g_symbol_table["_strcmp"]      = (void *)&strcmp;
    g_symbol_table["_strcpy"]      = (void *)&strcpy;
    g_symbol_table["_strdup"]      = (void *)&strdup;
    g_symbol_table["_strerror"]    = (void *)&strerror;
    g_symbol_table["_strlen"]      = (void *)&strlen;
    g_symbol_table["_strncasecmp"] = (void *)&strncasecmp;
    g_symbol_table["_strncat"]     = (void *)&strncat;
    g_symbol_table["_strncmp"]     = (void *)&strncmp;
    g_symbol_table["_strncpy"]     = (void *)&strncpy;
    g_symbol_table["_strrchr"]     = (void *)&darwin_strrchr;
    g_symbol_table["_strstr"]      = (void *)&darwin_strstr;
    g_symbol_table["_strtod"]      = (void *)&strtod;
    g_symbol_table["_strtok"]      = (void *)&strtok;
    g_symbol_table["_strtol"]      = (void *)&strtol;
    g_symbol_table["_strtoll"]     = (void *)&strtoll;
    g_symbol_table["_strtoul"]     = (void *)&strtoul;
    g_symbol_table["_strtoull"]    = (void *)&strtoull;
    g_symbol_table["_sysconf"]     = (void *)&darwin_sysconf;
    g_symbol_table["_sysctl"]      = (void *)&darwin_sysctl;
    g_symbol_table["_sysctlbyname"]= (void *)&darwin_sysctlbyname;
    g_symbol_table["_tan"]         = (void *)static_cast<double(*)(double)>(&tan);
    g_symbol_table["_tanf"]        = (void *)&tanf;
    g_symbol_table["_tcgetattr"]   = (void *)&tcgetattr;
    g_symbol_table["_tcsetattr"]   = (void *)&tcsetattr;
    g_symbol_table["_time"]        = (void *)&time;
    g_symbol_table["_tzset"]       = (void *)&tzset;
    g_symbol_table["_unlink"]      = (void *)&unlink;
    g_symbol_table["_usleep"]      = (void *)&usleep;
    g_symbol_table["_utime"]       = (void *)&utime;
    g_symbol_table["_write"]       = (void *)&write;

    /* Step 11: Self-verify that all 805 undefined symbols in undef_805_full.json resolve non-NULL */
    uint32_t resolved_805 = 0;
    for (size_t i = 0; i < 805; ++i) {
        if (g_symbol_table.find(kOh2All805UndefSymbols[i]) != g_symbol_table.end()) {
            resolved_805++;
        } else {
            oh2_log_error("Symbol resolver table missing undef[%zu]: %s", i, kOh2All805UndefSymbols[i]);
        }
    }
    oh2_log_info("Symbol resolver initialized: table_entries=%zu verified_undefs=%u/805",
                 g_symbol_table.size(), resolved_805);
}

extern "C" void *darwin_resolve_symbol(const char *dylib_path, const char *sym_name) {
    (void)dylib_path;
    if (!sym_name) return nullptr;
    auto it = g_symbol_table.find(sym_name);
    if (it != g_symbol_table.end()) {
        return it->second;
    }
    return nullptr;
}

static void register_macho_method_list(DarwinObjCClass *cls, const DarwinMethodList *mlist) {
    if (!cls || !mlist) return;
    uint32_t count = mlist->count;
    for (uint32_t i = 0; i < count; ++i) {
        const DarwinMethodEntry &m = mlist->entries[i];
        if (m.name && m.imp) {
            register_method_locked(cls, m.name, m.imp);
        }
    }
}

extern "C" int oh2_objc_register_image(MachOImage *img) {
    if (!img) return -1;
    std::lock_guard<std::mutex> lk(g_objc_mutex);

    /* 1. Intern all 1,491 selectors in __DATA,__objc_selrefs */
    const MachOSection *sel_sec = macho_find_section(img, "__DATA", "__objc_selrefs");
    if (sel_sec && sel_sec->mapped_addr) {
        const char **sels = (const char **)sel_sec->mapped_addr;
        uint32_t count = (uint32_t)(sel_sec->size / sizeof(void *));
        for (uint32_t i = 0; i < count; ++i) {
            if (sels[i]) {
                sels[i] = intern_selector(sels[i]);
            }
        }
        img->objc_selref_count = count;
    }

    /* 2. Register all 60 local classes in __DATA,__objc_classlist (and their metaclasses) */
    const MachOSection *cls_sec = macho_find_section(img, "__DATA", "__objc_classlist");
    if (cls_sec && cls_sec->mapped_addr) {
        DarwinObjCClass **cls_list = (DarwinObjCClass **)cls_sec->mapped_addr;
        uint32_t count = (uint32_t)(cls_sec->size / sizeof(void *));
        for (uint32_t i = 0; i < count; ++i) {
            DarwinObjCClass *cls = cls_list[i];
            if (!cls) continue;
            const DarwinClassRO *ro = (const DarwinClassRO *)(uintptr_t)(cls->data_ptr & ~0x7ULL);
            if (ro && ro->name) {
                g_class_by_name[ro->name] = cls;
            }
            if (ro && ro->baseMethodList) {
                register_macho_method_list(cls, ro->baseMethodList);
            }
            if (cls->isa && is_valid_class_ptr(cls->isa)) {
                const DarwinClassRO *mro = (const DarwinClassRO *)(uintptr_t)(cls->isa->data_ptr & ~0x7ULL);
                if (mro && mro->baseMethodList) {
                    register_macho_method_list(cls->isa, mro->baseMethodList);
                }
            }
        }
        img->objc_class_count = count;
    }

    /* 3. Register all 2 categories in __DATA,__objc_catlist */
    const MachOSection *cat_sec = macho_find_section(img, "__DATA", "__objc_catlist");
    if (cat_sec && cat_sec->mapped_addr) {
        DarwinCategory **cat_list = (DarwinCategory **)cat_sec->mapped_addr;
        uint32_t count = (uint32_t)(cat_sec->size / sizeof(void *));
        for (uint32_t i = 0; i < count; ++i) {
            DarwinCategory *cat = cat_list[i];
            if (!cat || !cat->cls) continue;
            if (cat->instanceMethods) {
                register_macho_method_list(cat->cls, cat->instanceMethods);
            }
            if (cat->classMethods && cat->cls->isa) {
                register_macho_method_list(cat->cls->isa, cat->classMethods);
            }
        }
        img->objc_cat_count = count;
    }

    /* 4. Count protocols in __DATA,__objc_protolist and CFStrings in __DATA,__cfstring */
    const MachOSection *proto_sec = macho_find_section(img, "__DATA", "__objc_protolist");
    if (proto_sec) {
        img->objc_proto_count = (uint32_t)(proto_sec->size / sizeof(void *));
    }
    const MachOSection *cfs_sec = macho_find_section(img, "__DATA", "__cfstring");
    if (cfs_sec) {
        img->cfstring_count = (uint32_t)(cfs_sec->size / sizeof(DarwinCFString));
    }

    oh2_log_info("ObjC static metadata registered: classes=%u categories=%u protocols=%u selrefs=%u cfstrings=%u",
                 img->objc_class_count,
                 img->objc_cat_count,
                 img->objc_proto_count,
                 img->objc_selref_count,
                 img->cfstring_count);
    return 0;
}
