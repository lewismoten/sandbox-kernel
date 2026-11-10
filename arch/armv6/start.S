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
/* Exceptions from User mode save the program's registers in the frame that
   user_enter was given (struct uframe: r0-r12, sp, lr, pc, cpsr) and return
   from user_enter with a reason; anything from a privileged mode is a panic,
   as in M0. The vector's lr adjustment makes pc the instruction to resume
   (svc, irq) or the one that faulted (undef, prefetch, data abort). */
.macro FROM_USER reason, adjust, stack, kind
    sub lr, lr, #\adjust
    str r0, [sp, #-4]!
    mrs r0, spsr
    and r0, r0, #0x1f
    cmp r0, #0x10
    ldr r0, [sp], #4
    movne r0, #\kind
    bne panic
    ldr sp, =user_frame_ptr
    ldr sp, [sp]
    stmia sp, {r0-r12}
    str lr, [sp, #60]
    mrs r0, spsr
    str r0, [sp, #64]
    add r0, sp, #52
    stmia r0, {sp, lr}^
    nop
    mov r0, #\reason
    ldr sp, =\stack
    b user_exit
.endm
undef: FROM_USER 3, 4, __undef_stack, 1
svc: FROM_USER 1, 0, __svc_stack, 2
prefetch: FROM_USER 4, 4, __abort_stack, 3
abort: FROM_USER 5, 8, __abort_stack, 4
reserved: mov r0, #5
    b panic
fiq: mov r0, #7
    b panic
irq:
    sub lr, lr, #4
    stmdb sp!, {r0-r3,r12,lr}
    mrs r0, spsr
    and r0, r0, #0x1f
    cmp r0, #0x10
    beq irq_user
    bl irq_handler
    ldmia sp!, {r0-r3,r12,pc}^
irq_user:
    /* Leave the interrupt pending: it is taken again in SVC mode once the
       kernel unmasks IRQs after user_enter returns. */
    ldmia sp!, {r0-r3,r12,lr}
    ldr sp, =user_frame_ptr
    ldr sp, [sp]
    stmia sp, {r0-r12}
    str lr, [sp, #60]
    mrs r0, spsr
    str r0, [sp, #64]
    add r0, sp, #52
    stmia r0, {sp, lr}^
    nop
    mov r0, #2
    ldr sp, =__irq_stack
    b user_exit

/* unsigned user_enter(struct uframe *frame, uint32_t user_ttbr): run the
   program until its next exception. The privileged table comes back before
   the kernel's own registers do. */
.global user_enter
user_enter:
    push {r4-r12, lr}
    cpsid i
    ldr r2, =kernel_sp
    str sp, [r2]
    ldr r2, =user_frame_ptr
    str r0, [r2]
    mrc p15, 0, r2, c2, c0, 0
    ldr r3, =kernel_ttbr
    str r2, [r3]
    mcr p15, 0, r1, c2, c0, 0
    mov r2, #0
    mcr p15, 0, r2, c7, c10, 4
    mcr p15, 0, r2, c8, c7, 0
    mcr p15, 0, r2, c7, c5, 6
    mcr p15, 0, r2, c7, c5, 4
    ldr r1, [r0, #64]
    msr spsr_cxsf, r1
    add r1, r0, #52
    ldmia r1, {sp, lr}^
    nop
    ldr lr, [r0, #60]
    ldmia r0, {r0-r12}
    movs pc, lr
user_exit:
    cps #0x13
    ldr r1, =kernel_ttbr
    ldr r1, [r1]
    mcr p15, 0, r1, c2, c0, 0
    mov r2, #0
    mcr p15, 0, r2, c7, c10, 4
    mcr p15, 0, r2, c8, c7, 0
    mcr p15, 0, r2, c7, c5, 6
    mcr p15, 0, r2, c7, c5, 4
    ldr sp, =kernel_sp
    ldr sp, [sp]
    pop {r4-r12, lr}
    bx lr
.section .bss
.balign 4
kernel_sp: .word 0
kernel_ttbr: .word 0
user_frame_ptr: .word 0

#ifndef SELECTOR
/* Relocated to 0x7800 before use. No stack, literals or calls after the runtime
   starts being overwritten. Source is in runtime BSS above the destination;
   forward copying is safe even if the ranges overlap. r3 is the new entry. */
.section .text.companion_copy,"ax"
.global companion_copy_start, companion_copy_end
companion_copy_start:
    cpsid if
    mov r4, #0
    mcr p15, 0, r4, c7, c14, 0
    mcr p15, 0, r4, c7, c10, 4
    mrc p15, 0, r5, c1, c0, 0
    bic r5, r5, #0x1000
    bic r5, r5, #5
    mcr p15, 0, r5, c1, c0, 0
    mcr p15, 0, r4, c7, c5, 4
1:  ldrb r4, [r0], #1
    strb r4, [r1], #1
    subs r2, r2, #1
    bne 1b
    mov r4, #0
    mcr p15, 0, r4, c7, c5, 0
    mcr p15, 0, r4, c7, c10, 4
    mcr p15, 0, r4, c7, c5, 4
    bx r3
companion_copy_end:
#endif
