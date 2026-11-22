#include "macho_loader.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static MachOImage g_img;

static void crash_signal_handler(int sig, siginfo_t *si, void *ucontext_raw) {
    ucontext_t *uc = (ucontext_t *)ucontext_raw;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
    uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
    uintptr_t fault_addr = (uintptr_t)(si ? si->si_addr : NULL);
    uintptr_t base = (uintptr_t)g_img.base_addr;

    uintptr_t pc_macho = (base && pc >= base && pc < base + g_img.mapped_span)
                         ? (0x100000000ULL + (pc - base))
                         : 0;
    uintptr_t lr_macho = (base && lr >= base && lr < base + g_img.mapped_span)
                         ? (0x100000000ULL + (lr - base))
                         : 0;

    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
    uintptr_t caller_lr1 = (fp > 0x10000ULL && (fp & 7) == 0) ? ((uintptr_t *)fp)[1] : 0;
    uintptr_t fp2        = (fp > 0x10000ULL && (fp & 7) == 0) ? ((uintptr_t *)fp)[0] : 0;
    uintptr_t caller_lr2 = (fp2 > 0x10000ULL && (fp2 & 7) == 0) ? ((uintptr_t *)fp2)[1] : 0;
    uintptr_t fp3        = (fp2 > 0x10000ULL && (fp2 & 7) == 0) ? ((uintptr_t *)fp2)[0] : 0;
    uintptr_t caller_lr3 = (fp3 > 0x10000ULL && (fp3 & 7) == 0) ? ((uintptr_t *)fp3)[1] : 0;

    oh2_log_error("CRASH signal=%d (%s) fault_addr=0x%lx pc=0x%lx (macho=0x%lx) lr=0x%lx (macho=0x%lx) sp=0x%lx mod_init_idx=%u mod_init_fn=%p",
                  sig, strsignal(sig),
                  (unsigned long)fault_addr,
                  (unsigned long)pc, (unsigned long)pc_macho,
                  (unsigned long)lr, (unsigned long)lr_macho,
                  (unsigned long)sp,
                  g_img.current_mod_init_idx,
                  g_img.current_mod_init_fn);
    oh2_log_error("FRAME CHAIN: fp=0x%lx lr1=0x%lx lr2=0x%lx lr3=0x%lx x0=0x%lx x1=0x%lx x8=0x%lx x16=0x%lx x19=0x%lx x20=0x%lx",
                  (unsigned long)fp,
                  (unsigned long)caller_lr1,
                  (unsigned long)caller_lr2,
                  (unsigned long)caller_lr3,
                  (unsigned long)uc->uc_mcontext.regs[0],
                  (unsigned long)uc->uc_mcontext.regs[1],
                  (unsigned long)uc->uc_mcontext.regs[8],
                  (unsigned long)uc->uc_mcontext.regs[16],
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
    const char *binary_path = (argc > 1) ? argv[1] : "/data/local/tmp/OH2";
    install_signal_handlers();

    oh2_log_info("=== Milestone M1 Headless Mach-O ARM64 Runtime Loader ===");
    oh2_log_info("Target Mach-O binary: %s", binary_path);

    int rc = macho_load(binary_path, &g_img);
    if (rc != 0) {
        oh2_log_error("[M1_FAIL] macho_load failed with rc=%d", rc);
        return 1;
    }

    void *mh_export = macho_lookup_export(&g_img, "__mh_execute_header");
    oh2_log_info("Export trie probe: __mh_execute_header=%p (base=%p)", mh_export, g_img.base_addr);

    if (g_img.cryptid != 0 ||
        g_img.tpidrro_el0_reads != 0 ||
        g_img.rebase_count != 520119 ||
        g_img.bind_count != 4040 ||
        g_img.bind_addend16_count != 219 ||
        g_img.lazy_bind_count != 539 ||
        g_img.weak_bind_count != 672 ||
        g_img.missing_symbol_count != 0 ||
        g_img.eh_frame_fde_count != 19 ||
        g_img.synth_fde_count != 3951 ||
        g_img.objc_class_count != 60) {
        oh2_log_error("[M1_FAIL] Metric mismatch: cryptid=%u tpidrro=%u rebase=%u bind=%u (add16=%u) lazy=%u weak=%u missing=%u eh_fdes=%u synth_fdes=%u objc_cls=%u",
                      g_img.cryptid,
                      g_img.tpidrro_el0_reads,
                      g_img.rebase_count,
                      g_img.bind_count,
                      g_img.bind_addend16_count,
                      g_img.lazy_bind_count,
                      g_img.weak_bind_count,
                      g_img.missing_symbol_count,
                      g_img.eh_frame_fde_count,
                      g_img.synth_fde_count,
                      g_img.objc_class_count);
        return 2;
    }

    oh2_log_info("[M1_PASS] 1_FAIRPLAY_AND_MMAP: cryptid=0 base=%p span=0x%zx tpidrro_el0_reads=0",
                 g_img.base_addr, g_img.mapped_span);
    oh2_log_info("[M1_PASS] 2_DYLD_FIXUPS: rebase=%u bind=%u addend16=%u lazy_bind=%u weak_bind=%u missing_symbols=%u",
                 g_img.rebase_count,
                 g_img.bind_count,
                 g_img.bind_addend16_count,
                 g_img.lazy_bind_count,
                 g_img.weak_bind_count,
                 g_img.missing_symbol_count);
    oh2_log_info("[M1_PASS] 3_UNWIND_AND_OBJC: eh_frame_fdes=%u synth_fdes=%u lsdas=%u objc_classes=%u objc_cats=%u selrefs=%u cfstrings=%u",
                 g_img.eh_frame_fde_count,
                 g_img.synth_fde_count,
                 g_img.lsda_count,
                 g_img.objc_class_count,
                 g_img.objc_cat_count,
                 g_img.objc_selref_count,
                 g_img.cfstring_count);

    char arg0[512];
    snprintf(arg0, sizeof(arg0), "%s", binary_path);
    char *d_argv[2] = { arg0, NULL };
    char *d_envp[3] = {
        (char *)"HOME=/data/local/tmp",
        (char *)"TMPDIR=/data/local/tmp",
        NULL
    };
    char apple_exec[560];
    snprintf(apple_exec, sizeof(apple_exec), "executable_path=%s", binary_path);
    char *d_apple[2] = { apple_exec, NULL };

    if (macho_run_initializers(&g_img, 1, d_argv, d_envp, d_apple) != 0 ||
        g_img.mod_init_executed != 466) {
        oh2_log_error("[M1_FAIL] __mod_init_func execution failed (executed=%u/%u)",
                      g_img.mod_init_executed, g_img.mod_init_count);
        return 3;
    }
    oh2_log_info("[M1_PASS] 4_MOD_INIT_FUNCS: executed=%u/%u",
                 g_img.mod_init_executed, g_img.mod_init_count);

    int main_rc = macho_call_main(&g_img, 1, d_argv, d_envp, d_apple);
    if (main_rc != 0) {
        oh2_log_error("[M1_FAIL] LC_MAIN returned non-zero: %d", main_rc);
        return 4;
    }

    oh2_log_info("[M1_PASS] 5_LC_MAIN_UE4_STARTUP: entryoff=0x%llx return_code=%d",
                 (unsigned long long)g_img.entry_offset, main_rc);
    oh2_log_info("[M1_STATE] M1_LOADER_VERIFIED_OK");

    macho_unload(&g_img);
    return 0;
}
