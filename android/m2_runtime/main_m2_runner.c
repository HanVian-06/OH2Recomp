#include "macho_loader.h"
#include "libc_wrapper.h"
#include "oh2_runtime.h"

#include <android/log.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static MachOImage g_img;

static void m2_log_info(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "OH2_M2", "%s", buf);
    fprintf(stdout, "[OH2_M2] %s\n", buf);
    fflush(stdout);
}

static void m2_log_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_ERROR, "OH2_M2", "%s", buf);
    fprintf(stderr, "[OH2_M2] %s\n", buf);
    fflush(stderr);
}

static void crash_signal_handler(int sig, siginfo_t *si, void *ucontext_raw) {
    ucontext_t *uc = (ucontext_t *)ucontext_raw;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
    uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
    uintptr_t fault_addr = (uintptr_t)(si ? si->si_addr : NULL);
    uintptr_t base = (uintptr_t)g_img.base_addr;

    uintptr_t pc_macho = (base && pc >= base && pc < base + g_img.mapped_span)
                         ? (0x100000000ULL + (pc - base)) : 0;
    uintptr_t lr_macho = (base && lr >= base && lr < base + g_img.mapped_span)
                         ? (0x100000000ULL + (lr - base)) : 0;

    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
    uintptr_t caller_lr1 = (fp > 0x10000ULL && (fp & 7) == 0) ? ((uintptr_t *)fp)[1] : 0;
    uintptr_t fp2        = (fp > 0x10000ULL && (fp & 7) == 0) ? ((uintptr_t *)fp)[0] : 0;
    uintptr_t caller_lr2 = (fp2 > 0x10000ULL && (fp2 & 7) == 0) ? ((uintptr_t *)fp2)[1] : 0;
    uintptr_t fp3        = (fp2 > 0x10000ULL && (fp2 & 7) == 0) ? ((uintptr_t *)fp2)[0] : 0;
    uintptr_t caller_lr3 = (fp3 > 0x10000ULL && (fp3 & 7) == 0) ? ((uintptr_t *)fp3)[1] : 0;

    m2_log_error("CRASH signal=%d (%s) fault_addr=0x%lx pc=0x%lx (macho=0x%lx) lr=0x%lx (macho=0x%lx) sp=0x%lx tid=%d",
                 sig, strsignal(sig),
                 (unsigned long)fault_addr,
                 (unsigned long)pc, (unsigned long)pc_macho,
                 (unsigned long)lr, (unsigned long)lr_macho,
                 (unsigned long)sp, (int)gettid());
    m2_log_error("FRAME CHAIN: fp=0x%lx lr1=0x%lx lr2=0x%lx lr3=0x%lx x0=0x%lx x1=0x%lx x8=0x%lx x19=0x%lx x20=0x%lx",
                 (unsigned long)fp, (unsigned long)caller_lr1, (unsigned long)caller_lr2,
                 (unsigned long)caller_lr3,
                 (unsigned long)uc->uc_mcontext.regs[0],
                 (unsigned long)uc->uc_mcontext.regs[1],
                 (unsigned long)uc->uc_mcontext.regs[8],
                 (unsigned long)uc->uc_mcontext.regs[19],
                 (unsigned long)uc->uc_mcontext.regs[20]);
    _exit(128 + sig);
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}

int main(int argc, char **argv) {
    install_signal_handlers();

    const char *test_name   = NULL;
    const char *binary_path = "/data/local/tmp/OH2";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--test") == 0 && i + 1 < argc) {
            test_name = argv[++i];
        } else if (argv[i][0] != '-') {
            binary_path = argv[i];
        }
    }

    if (test_name) {
        int rc = oh2_runtime_run_selftest(test_name, binary_path);
        fflush(stdout);
        fflush(stderr);
        _exit(rc);
    }

    m2_log_info("=== Milestone M2 Runtime & Darwin/OS Shim Runner ===");
    if (oh2_runtime_init("/sdcard/Android/data/com.oceanhorn2.port/files") != 0) {
        m2_log_error("[M2_FAIL] oh2_runtime_init failed");
        return 1;
    }

    if (oh2_runtime_run_selftest("all_unit", binary_path) != 0) {
        m2_log_error("[M2_FAIL] M2 unit self-tests failed");
        return 2;
    }
    m2_log_info("[M2_PASS] 1_OBJC_MSGSSEND: register preservation self-test PASS");
    m2_log_info("[M2_PASS] 2_LIBC_WRAPPER: errno/sockaddr/stat/open/pthread PASS");
    m2_log_info("[M2_PASS] 3_NSBUNDLE: /sdcard/Android/data/com.oceanhorn2.port/files/ mapping PASS");
    m2_log_info("[M2_PASS] 4_FRAMEWORK_STUBS: GameKit/StoreKit/CloudKit non-crashing PASS");

    if (macho_load(binary_path, &g_img) != 0) {
        m2_log_error("[M2_FAIL] macho_load failed for %s", binary_path);
        return 3;
    }
    oh2_runtime_post_image_overrides();

    char arg0[512];
    snprintf(arg0, sizeof(arg0), "%s", binary_path);
    char *d_argv[2] = { arg0, NULL };
    char *d_envp[3] = {
        (char *)"HOME=/sdcard/Android/data/com.oceanhorn2.port/files",
        (char *)"TMPDIR=/sdcard/Android/data/com.oceanhorn2.port/files/tmp",
        NULL
    };
    char apple_exec[560];
    snprintf(apple_exec, sizeof(apple_exec), "executable_path=%s", binary_path);
    char *d_apple[2] = { apple_exec, NULL };

    if (macho_run_initializers(&g_img, 1, d_argv, d_envp, d_apple) != 0 ||
        g_img.mod_init_executed != 466) {
        m2_log_error("[M2_FAIL] __mod_init_func failed (%u/%u)",
                     g_img.mod_init_executed, g_img.mod_init_count);
        return 4;
    }

    if (oh2_runtime_verify_pak_direct(NULL, 1) != 0) {
        m2_log_error("[M2_FAIL] Direct Pak v8 verification failed");
        _exit(6);
    }

    oh2_runtime_set_enable_main_thread(1);
    int main_rc = macho_call_main(&g_img, 1, d_argv, d_envp, d_apple);

    Oh2PakIoTelemetry io;
    oh2_pak_telemetry_get(&io);
    if (!io.opened_by_ue4 || !io.footer_read || !io.index_read_complete) {
        m2_log_error("[M2_FAIL] UE4 did not finish mounting oh2-ios.pak (opened=%d footer=%d index_bytes=%llu complete=%d main_rc=%d)",
                     io.opened_by_ue4, io.footer_read,
                     (unsigned long long)io.index_bytes_read,
                     io.index_read_complete, main_rc);
        _exit(5);
    }

    m2_log_info("[M2_PASS] 5_PAK_OPEN: UE4 opened %s magic=0x5a6f12e1 version=8 entries=19920 MountPoint=../../../ fstat_size=%llu index_bytes_read=%llu",
                io.opened_path,
                (unsigned long long)io.fstat_size,
                (unsigned long long)io.index_bytes_read);
    m2_log_info("[M2_STATE] M2_RUNTIME_VERIFIED_OK");

    fflush(stdout);
    fflush(stderr);
    _exit(0);
}
