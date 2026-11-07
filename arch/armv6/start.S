/* SPDX-License-Identifier: MIT; ARM1176 ARM-state firmware entry. */
.syntax unified
.cpu arm1176jzf-s
.arm
.section .text.boot,"ax"
.global _start
_start:
    cpsid if
    /* Firmware enters with identity addressing. Clean inherited dirty data
       before disabling the MMU; invalidate stale instructions and TLBs. */
    mov r0, #0
    mcr p15, 0, r0, c7, c14, 0
    mcr p15, 0, r0, c7, c10, 4
    mrc p15, 0, r1, c1, c0, 0
    bic r1, r1, #0x1000
    bic r1, r1, #0x0005
    bic r1, r1, #0x2000
    bic r1, r1, #0x82       /* little endian, alignment fault disabled */
    orr r1, r1, #0x400000   /* ARMv6 unaligned model, compiler uses byte access */
    mcr p15, 0, r1, c1, c0, 0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c8, c7, 0
    mcr p15, 0, r0, c7, c5, 4
    cps #0x12
    ldr sp, =__irq_stack
    cps #0x11
    ldr sp, =__fiq_stack
    cps #0x17
    ldr sp, =__abort_stack
    cps #0x1b
    ldr sp, =__undef_stack
    cps #0x13
    ldr sp, =__svc_stack
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    ldr r0, =vectors
    mov r1, #0
    mov r2, #64
2:  ldr r3, [r0], #4
    str r3, [r1], #4
    subs r2, r2, #4
    bne 2b
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c10, 4
    mcr p15, 0, r0, c7, c5, 4
#ifdef SELECTOR
    bl selector_main
#else
    bl kernel_main
#endif
3:  b 3b
.balign 32
vectors:
    ldr pc, [pc, #24]
    ldr pc, [pc, #24]
    ldr pc, [pc, #24]
    ldr pc, [pc, #24]
    ldr pc, [pc, #24]
    ldr pc, [pc, #24]
    ldr pc, [pc, #24]
    ldr pc, [pc, #24]
    .word _start, undef, svc, prefetch, abort, reserved, irq, fiq
undef: mov r0, #1
    b panic
svc: mov r0, #2
    b panic
prefetch: mov r0, #3
    b panic
abort: mov r0, #4
    b panic
reserved: mov r0, #5
    b panic
fiq: mov r0, #7
    b panic
irq:
    sub lr, lr, #4
    stmdb sp!, {r0-r3,r12,lr}
    bl irq_handler
    ldmia sp!, {r0-r3,r12,pc}^
