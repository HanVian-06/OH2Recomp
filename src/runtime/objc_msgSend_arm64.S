.text
.align 4

/*
 * M2 Darwin ARM64 Objective-C Message Dispatch & ABI Preservation Self-Test Probes
 *
 * Frame layout (224 bytes, 16-byte aligned):
 *   [sp, #0..15]    x29, x30
 *   [sp, #16..31]   x0, x1
 *   [sp, #32..47]   x2, x3
 *   [sp, #48..63]   x4, x5
 *   [sp, #64..79]   x6, x7
 *   [sp, #80..95]   x8 (+8 byte pad)
 *   [sp, #96..127]  q0, q1
 *   [sp, #128..159] q2, q3
 *   [sp, #160..191] q4, q5
 *   [sp, #192..223] q6, q7
 */

.global oh2_objc_msgSend
.type oh2_objc_msgSend, %function
oh2_objc_msgSend:
    cbz  x0, .Loh2_nil_recv
    cbz  x1, .Loh2_msg_slow
    ldr  x9, [x0]
    tbnz x9, #0, .Loh2_msg_slow
    cmp  x9, #0x10000
    b.lo .Loh2_msg_slow
    lsr  x10, x9, #4
    eor  x10, x10, x1, lsr #2
    eor  x10, x10, x1, lsr #11
    and  x10, x10, #0x3fff
    adrp x11, g_objc_fast_cache
    add  x11, x11, :lo12:g_objc_fast_cache
    add  x11, x11, x10, lsl #5
    add  x15, x11, #16
    ldar x16, [x15]
    cbz  x16, .Loh2_msg_slow
    ldp  x12, x13, [x11]
    ldr  x14, [x11, #24]
    adrp x15, g_objc_fast_cache_epoch
    ldr  x15, [x15, :lo12:g_objc_fast_cache_epoch]
    cmp  x12, x9
    ccmp x13, x1, #0, eq
    ccmp x14, x15, #0, eq
    b.ne .Loh2_msg_slow
    br   x16
.Loh2_msg_slow:
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8,     [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    bl  oh2_objc_lookup_imp
    mov x16, x0
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldr x8,     [sp, #80]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    ldp x29, x30, [sp], #224
    br  x16
.size oh2_objc_msgSend, . - oh2_objc_msgSend

.global oh2_objc_msgSendSuper2
.type oh2_objc_msgSendSuper2, %function
oh2_objc_msgSendSuper2:
    cbz x0, .Loh2_nil_recv
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8,     [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    bl  oh2_objc_lookup_super2_imp
    mov x16, x0
    ldp x0, x1, [sp, #16]
    ldr x0, [x0]            /* x0 = objc_super->receiver */
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldr x8,     [sp, #80]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    ldp x29, x30, [sp], #224
    cbz x0, .Loh2_nil_recv
    br  x16
.size oh2_objc_msgSendSuper2, . - oh2_objc_msgSendSuper2

.global oh2_objc_msgSend_stret
.type oh2_objc_msgSend_stret, %function
oh2_objc_msgSend_stret:
    b oh2_objc_msgSend
.size oh2_objc_msgSend_stret, . - oh2_objc_msgSend_stret

.Loh2_nil_recv:
    mov  x0, #0
    mov  x1, #0
    mov  x2, #0
    mov  x3, #0
    movi v0.2d, #0
    movi v1.2d, #0
    movi v2.2d, #0
    movi v3.2d, #0
    ret

/*
 * ABI Capture IMP:
 * Receives x0..x8, d0..d7, and 2 stack words [sp,#0], [sp,#8].
 * x8 points to CapturedAbiState (18 uint64_t fields = 144 bytes):
 *   +0..+64  : x0..x8
 *   +72..+128: d0..d7
 *   +136     : stack[0]
 *   +144     : stack[1]
 */
.global oh2_asm_abi_capture_imp
.type oh2_asm_abi_capture_imp, %function
oh2_asm_abi_capture_imp:
    stp x0, x1, [x8, #0]
    stp x2, x3, [x8, #16]
    stp x4, x5, [x8, #32]
    stp x6, x7, [x8, #48]
    str x8,     [x8, #64]
    stp d0, d1, [x8, #72]
    stp d2, d3, [x8, #88]
    stp d4, d5, [x8, #104]
    stp d6, d7, [x8, #120]
    ldp x9, x10, [sp, #0]
    stp x9, x10, [x8, #136]
    movz x0, #0x5678
    movk x0, #0x1234, lsl #16
    movk x0, #0xBABE, lsl #32
    movk x0, #0xCAFE, lsl #48
    ret
.size oh2_asm_abi_capture_imp, . - oh2_asm_abi_capture_imp

/*
 * int64_t oh2_asm_msgsend_abi_probe(void *obj_or_super, const char *sel, void *capture_out, int is_super)
 */
.global oh2_asm_msgsend_abi_probe
.type oh2_asm_msgsend_abi_probe, %function
oh2_asm_msgsend_abi_probe:
    sub  sp, sp, #32
    stp  x29, x30, [sp, #16]
    add  x29, sp, #16
    /* Put stack sentinel arguments at [sp,#0] and [sp,#8] */
    movz x9, #0x4444
    movk x9, #0x3333, lsl #16
    movk x9, #0x2222, lsl #32
    movk x9, #0x1111, lsl #48
    movz x10, #0x8888
    movk x10, #0x7777, lsl #16
    movk x10, #0x6666, lsl #32
    movk x10, #0x5555, lsl #48
    stp x9, x10, [sp, #0]

    /* Preserve dispatch selector flag and out buffer */
    mov x11, x3
    mov x8,  x2

    /* Load distinct GP constants into x2..x7 */
    movz x2, #0x0002
    movk x2, #0xA2A2, lsl #48
    movz x3, #0x0003
    movk x3, #0xA3A3, lsl #48
    movz x4, #0x0004
    movk x4, #0xA4A4, lsl #48
    movz x5, #0x0005
    movk x5, #0xA5A5, lsl #48
    movz x6, #0x0006
    movk x6, #0xA6A6, lsl #48
    movz x7, #0x0007
    movk x7, #0xA7A7, lsl #48

    /* Load distinct double bit-patterns into d0..d7 */
    movz x12, #0x3FF0, lsl #48
    fmov d0, x12
    movz x12, #0x4000, lsl #48
    fmov d1, x12
    movz x12, #0x4008, lsl #48
    fmov d2, x12
    movz x12, #0x4010, lsl #48
    fmov d3, x12
    movz x12, #0x4014, lsl #48
    fmov d4, x12
    movz x12, #0x4018, lsl #48
    fmov d5, x12
    movz x12, #0x401C, lsl #48
    fmov d6, x12
    movz x12, #0x4020, lsl #48
    fmov d7, x12

    cbnz x11, .Lprobe_super2
    bl   oh2_objc_msgSend
    b    .Lprobe_done
.Lprobe_super2:
    bl   oh2_objc_msgSendSuper2
.Lprobe_done:
    ldp  x29, x30, [sp, #16]
    add  sp, sp, #32
    ret
.size oh2_asm_msgsend_abi_probe, . - oh2_asm_msgsend_abi_probe
