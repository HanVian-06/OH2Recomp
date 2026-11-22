.text
.align 4

/*
 * Darwin ARM64 Variadic -> Android AAPCS64 Trampolines
 *
 * On Darwin ARM64, all variadic arguments (GP and FP) are placed on the stack
 * in 8-byte slots starting at [sp, #0], [sp, #8], ...
 * By copying the entry SP into the first free argument register x<K> without
 * modifying SP or LR (x30), the C handler constructs an AAPCS64 va_list with
 * __stack = darwin_sp, __gr_offs = 0, __vr_offs = 0 in O(1).
 */
.macro DARWIN_VARIADIC_TRAMPOLINE name, target_c, k_reg
    .global \name
    .type \name, %function
\name:
    mov \k_reg, sp
    b \target_c
    .size \name, . - \name
.endm

/* 12 C/Foundation Variadic Imports in Payload/OH2.app/OH2 */
DARWIN_VARIADIC_TRAMPOLINE darwin_printf,        darwin_printf_impl,        x1
DARWIN_VARIADIC_TRAMPOLINE darwin_NSLog,         darwin_NSLog_impl,         x1
DARWIN_VARIADIC_TRAMPOLINE darwin_syscall,       darwin_syscall_impl,       x1
DARWIN_VARIADIC_TRAMPOLINE darwin_fprintf,       darwin_fprintf_impl,       x2
DARWIN_VARIADIC_TRAMPOLINE darwin_sprintf,       darwin_sprintf_impl,       x2
DARWIN_VARIADIC_TRAMPOLINE darwin_sscanf,        darwin_sscanf_impl,        x2
DARWIN_VARIADIC_TRAMPOLINE darwin_open,          darwin_open_impl,          x2
DARWIN_VARIADIC_TRAMPOLINE darwin_fcntl,         darwin_fcntl_impl,         x2
DARWIN_VARIADIC_TRAMPOLINE darwin_ioctl,         darwin_ioctl_impl,         x2
DARWIN_VARIADIC_TRAMPOLINE darwin_snprintf,      darwin_snprintf_impl,      x3
DARWIN_VARIADIC_TRAMPOLINE darwin_sprintf_chk,   darwin_sprintf_chk_impl,   x4
DARWIN_VARIADIC_TRAMPOLINE darwin_snprintf_chk,  darwin_snprintf_chk_impl,  x5

/* ObjC variadic selectors: x0=self, x1=_cmd, x2=format, [sp]=varargs */
DARWIN_VARIADIC_TRAMPOLINE darwin_objc_stringWithFormat, darwin_objc_stringWithFormat_impl, x3
DARWIN_VARIADIC_TRAMPOLINE darwin_objc_appendFormat,     darwin_objc_appendFormat_impl,     x3

/*
 * Objective-C Message Dispatch Trampolines (ARM64)
 * Preserves x0..x8 and d0..d7 across method lookup and restores caller SP
 * before tail-branching to the resolved IMP.
 */
.global darwin_objc_msgSend
.type darwin_objc_msgSend, %function
darwin_objc_msgSend:
    cbz x0, .Lobjc_nil_receiver
    stp x29, x30, [sp, #-160]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8,     [sp, #80]
    stp d0, d1, [sp, #96]
    stp d2, d3, [sp, #112]
    stp d4, d5, [sp, #128]
    stp d6, d7, [sp, #144]
    bl darwin_objc_lookup_imp
    mov x16, x0
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldr x8,     [sp, #80]
    ldp d0, d1, [sp, #96]
    ldp d2, d3, [sp, #112]
    ldp d4, d5, [sp, #128]
    ldp d6, d7, [sp, #144]
    ldp x29, x30, [sp], #160
    br x16
.size darwin_objc_msgSend, . - darwin_objc_msgSend

.global darwin_objc_msgSendSuper2
.type darwin_objc_msgSendSuper2, %function
darwin_objc_msgSendSuper2:
    cbz x0, .Lobjc_nil_receiver
    stp x29, x30, [sp, #-160]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8,     [sp, #80]
    stp d0, d1, [sp, #96]
    stp d2, d3, [sp, #112]
    stp d4, d5, [sp, #128]
    stp d6, d7, [sp, #144]
    bl darwin_objc_lookup_super2_imp
    mov x16, x0
    ldp x0, x1, [sp, #16]
    ldr x0, [x0]            /* x0 = objc_super->receiver */
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldr x8,     [sp, #80]
    ldp d0, d1, [sp, #96]
    ldp d2, d3, [sp, #112]
    ldp d4, d5, [sp, #128]
    ldp d6, d7, [sp, #144]
    ldp x29, x30, [sp], #160
    cbz x0, .Lobjc_nil_receiver
    br x16

.Lobjc_nil_receiver:
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    movi d0, #0
    movi d1, #0
    movi d2, #0
    movi d3, #0
    ret
.size darwin_objc_msgSendSuper2, . - darwin_objc_msgSendSuper2
