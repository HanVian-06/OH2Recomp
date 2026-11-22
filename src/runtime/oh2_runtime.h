#ifndef OH2_RUNTIME_H
#define OH2_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize M2 runtime bridges, libc_wrapper overrides, ObjC dispatch ASM, and framework stubs */
int  oh2_runtime_init(const char *bundle_root);

/* Apply post-macho_load method overrides onto OH2's 60 local classes (e.g. PLCrashReporter) */
void oh2_runtime_post_image_overrides(void);

/* Enable or disable spawning real background pthreads from -[NSThread start] (off in M1, on in M2) */
void oh2_runtime_set_enable_main_thread(int enable);

/* Register/trigger internal bridge subsystems (idempotent) */
void oh2_m2_register_objc_runtime(void);
void oh2_m2_register_ui_bridges(void);
void oh2_m2_register_ns_bridges(void);
void oh2_m2_register_framework_stubs(void);

/* Signal & wait helpers for UE4 MainAppThread -> Pak mount boundary */
int  oh2_runtime_did_hit_metal_boundary(void);
void oh2_runtime_reset_metal_boundary(void);
void oh2_runtime_notify_main_thread_exited(void);
int  oh2_runtime_wait_for_pak_or_boundary(int timeout_ms);

/* Direct pak v8 verification using Darwin libc_wrapper (open/fstat/lseek/read + zlib) */
int  oh2_runtime_verify_pak_direct(const char *pak_path, int extract_verify);

/* Execute a named M2 self-test (for gate_m2.ps1 --test <name> and main gate run) */
int  oh2_runtime_run_selftest(const char *test_name, const char *oh2_path);

#ifdef __cplusplus
}
#endif

#endif /* OH2_RUNTIME_H */
