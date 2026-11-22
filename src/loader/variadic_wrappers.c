#include "macho_loader.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * Android NDK r28c AAPCS64 va_list layout (sizeof == 32 bytes).
 * Setting __gr_offs = 0 and __vr_offs = 0 forces va_arg() for both general
 * and floating-point registers to fetch sequentially in 8-byte steps from __stack.
 */
typedef struct {
    void *__stack;
    void *__gr_top;
    void *__vr_top;
    int   __gr_offs;
    int   __vr_offs;
} aapcs64_va_list_t;

_Static_assert(sizeof(aapcs64_va_list_t) == sizeof(va_list),
               "aapcs64_va_list_t must match NDK aarch64 va_list size (32 bytes)");

static inline void darwin_to_aapcs64_va_list(char *darwin_ap, va_list *out_va) {
    aapcs64_va_list_t v;
    v.__stack   = (void *)darwin_ap;
    v.__gr_top  = NULL;
    v.__vr_top  = NULL;
    v.__gr_offs = 0;
    v.__vr_offs = 0;
    memcpy(out_va, &v, sizeof(v));
}

static void mirror_ue4_log(const char *msg) {
    if (!msg || !msg[0]) return;
    __android_log_print(ANDROID_LOG_INFO, "OH2_UE4", "%s", msg);
    fprintf(stdout, "[OH2_UE4] %s\n", msg);
    fflush(stdout);
}

int darwin_vsnprintf_objc(char *out, size_t out_sz, const char *fmt, char *darwin_sp) {
    if (!fmt) {
        if (out && out_sz > 0) out[0] = '\0';
        return 0;
    }

    if (strstr(fmt, "%@") == NULL) {
        va_list ap;
        darwin_to_aapcs64_va_list(darwin_sp, &ap);
        return vsnprintf(out, out_sz, fmt, ap);
    }

    char rewritten_fmt[2048];
    uint64_t slots[16];
    char scratch_pool[16][256];
    if (darwin_sp) {
        memcpy(slots, darwin_sp, sizeof(slots));
    } else {
        memset(slots, 0, sizeof(slots));
    }

    size_t r_pos = 0;
    size_t slot_idx = 0;
    for (const char *p = fmt; *p && r_pos + 4 < sizeof(rewritten_fmt); ++p) {
        if (*p != '%') {
            rewritten_fmt[r_pos++] = *p;
            continue;
        }
        if (p[1] == '%') {
            rewritten_fmt[r_pos++] = '%';
            rewritten_fmt[r_pos++] = '%';
            ++p;
            continue;
        }
        if (p[1] == '@') {
            rewritten_fmt[r_pos++] = '%';
            rewritten_fmt[r_pos++] = 's';
            if (slot_idx < 16) {
                const void *obj = (const void *)(uintptr_t)slots[slot_idx];
                const char *utf8 = darwin_cfstring_utf8(
                    obj, scratch_pool[slot_idx], sizeof(scratch_pool[slot_idx]));
                slots[slot_idx] = (uint64_t)(uintptr_t)(utf8 ? utf8 : "(null)");
                slot_idx++;
            }
            ++p;
            continue;
        }

        /* Copy standard format specifier and advance slot_idx */
        rewritten_fmt[r_pos++] = *p++;
        while (*p && strchr("-+ #0123456789.hljztL", *p) && r_pos + 4 < sizeof(rewritten_fmt)) {
            if (*p == '*') slot_idx++;
            rewritten_fmt[r_pos++] = *p++;
        }
        if (*p && r_pos + 4 < sizeof(rewritten_fmt)) {
            rewritten_fmt[r_pos++] = *p;
            slot_idx++;
        } else {
            break;
        }
    }
    rewritten_fmt[r_pos] = '\0';

    va_list ap;
    darwin_to_aapcs64_va_list((char *)slots, &ap);
    return vsnprintf(out, out_sz, rewritten_fmt, ap);
}

int darwin_printf_impl(const char *fmt, char *darwin_sp) {
    char buf[2048];
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_sp, &ap);
    int n = vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    mirror_ue4_log(buf);
    return n;
}

int darwin_fprintf_impl(FILE *stream, const char *fmt, char *darwin_sp) {
    char buf[2048];
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_sp, &ap);
    int n = vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    if (stream == stdout || stream == stderr || stream == NULL) {
        mirror_ue4_log(buf);
    } else if (n > 0) {
        fwrite(buf, 1, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), stream);
    }
    return n;
}

int darwin_sprintf_impl(char *s, const char *fmt, char *darwin_sp) {
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_sp, &ap);
    return vsprintf(s, fmt, ap);
}

int darwin_snprintf_impl(char *s, size_t n, const char *fmt, char *darwin_sp) {
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_sp, &ap);
    return vsnprintf(s, n, fmt, ap);
}

int darwin_sprintf_chk_impl(char *s, int flag, size_t os, const char *fmt, char *darwin_sp) {
    (void)flag;
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_sp, &ap);
    if (os != (size_t)-1 && os != 0) {
        return vsnprintf(s, os, fmt, ap);
    }
    return vsprintf(s, fmt, ap);
}

int darwin_snprintf_chk_impl(char *s, size_t maxlen, int flag, size_t os, const char *fmt, char *darwin_sp) {
    (void)flag;
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_sp, &ap);
    size_t limit = (os != (size_t)-1 && os < maxlen) ? os : maxlen;
    return vsnprintf(s, limit, fmt, ap);
}

int darwin_sscanf_impl(const char *s, const char *fmt, char *darwin_sp) {
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_sp, &ap);
    return vsscanf(s, fmt, ap);
}

void darwin_NSLog_impl(const void *ns_fmt, char *darwin_sp) {
    char fmt_scratch[512];
    const char *fmt = darwin_cfstring_utf8(ns_fmt, fmt_scratch, sizeof(fmt_scratch));
    if (!fmt) fmt = "(null)";
    char out_buf[2048];
    darwin_vsnprintf_objc(out_buf, sizeof(out_buf), fmt, darwin_sp);
    mirror_ue4_log(out_buf);
}

static int translate_darwin_open_flags(int d_flags) {
    int l_flags = d_flags & O_ACCMODE;
    if (d_flags & 0x00000004) l_flags |= O_NONBLOCK;
    if (d_flags & 0x00000008) l_flags |= O_APPEND;
    if (d_flags & 0x00000080) l_flags |= O_SYNC;
    if (d_flags & 0x00000100) l_flags |= O_NOFOLLOW;
    if (d_flags & 0x00000200) l_flags |= O_CREAT;
    if (d_flags & 0x00000400) l_flags |= O_TRUNC;
    if (d_flags & 0x00000800) l_flags |= O_EXCL;
    if (d_flags & 0x00020000) l_flags |= O_NOCTTY;
    if (d_flags & 0x00100000) l_flags |= O_DIRECTORY;
    if (d_flags & 0x01000000) l_flags |= O_CLOEXEC;
    return l_flags;
}

int darwin_open_impl(const char *path, int d_flags, char *darwin_sp) {
    int l_flags = translate_darwin_open_flags(d_flags);
    mode_t mode = 0666;
    if ((d_flags & 0x00000200) && darwin_sp) {
        mode = (mode_t)(*(uint64_t *)darwin_sp);
    }
    return open(path, l_flags, mode);
}

int darwin_fcntl_impl(int fd, int cmd, char *darwin_sp) {
    uint64_t arg = darwin_sp ? (*(uint64_t *)darwin_sp) : 0;
    switch (cmd) {
        case 1: /* F_GETFD */
            return fcntl(fd, F_GETFD);
        case 2: /* F_SETFD */
            return fcntl(fd, F_SETFD, (int)arg);
        case 3: { /* F_GETFL */
            int fl = fcntl(fd, F_GETFL);
            if (fl < 0) return fl;
            int d_fl = fl & O_ACCMODE;
            if (fl & O_NONBLOCK) d_fl |= 0x0004;
            if (fl & O_APPEND)   d_fl |= 0x0008;
            return d_fl;
        }
        case 4: { /* F_SETFL */
            int l_fl = translate_darwin_open_flags((int)arg);
            return fcntl(fd, F_SETFL, l_fl);
        }
        case 51: /* Darwin F_FULLFSYNC */
            return fsync(fd);
        case 73: /* Darwin F_SETNOSIGPIPE */
            return 0;
        default:
            return fcntl(fd, cmd, arg);
    }
}

int darwin_ioctl_impl(int fd, unsigned long req, char *darwin_sp) {
    void *argp = darwin_sp ? (*(void **)darwin_sp) : NULL;
    unsigned long l_req = req;
    if (req == 0x4004667fUL) { /* Darwin FIONREAD */
        l_req = FIONREAD;
    } else if (req == 0x8004667eUL) { /* Darwin FIONBIO */
        l_req = FIONBIO;
    }
    return ioctl(fd, l_req, argp);
}

long darwin_syscall_impl(int num, char *darwin_sp) {
    (void)darwin_sp;
    if (num == 286 || num == 0x11e) { /* Darwin SYS_thread_selfid / gettid */
        return (long)gettid();
    }
    return (long)gettid();
}

int darwin_vfprintf(FILE *stream, const char *fmt, char *darwin_ap) {
    char buf[2048];
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_ap, &ap);
    int n = vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    if (stream == stdout || stream == stderr || stream == NULL) {
        mirror_ue4_log(buf);
    } else if (n > 0) {
        fwrite(buf, 1, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), stream);
    }
    return n;
}

int darwin_vsnprintf(char *s, size_t n, const char *fmt, char *darwin_ap) {
    va_list ap;
    darwin_to_aapcs64_va_list(darwin_ap, &ap);
    return vsnprintf(s, n, fmt, ap);
}
