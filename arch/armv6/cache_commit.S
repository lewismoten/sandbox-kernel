/* SPDX-License-Identifier: MIT; cooperative AAPCS stack switch. Called only
 * from SVC mode in the foreground with the privileged TTBR and IRQs enabled.
 * Soft-float: no VFP state. IRQs retain their own banked stack as usual. */
.syntax unified
.cpu arm1176jzf-s
.arm
.text
.balign 4
.global cache_commit_switch
.type cache_commit_switch,%function
cache_commit_switch:
    stmia r0, {r4-r11, sp, lr}
    ldmia r1, {r4-r11, sp, lr}
    bx lr
.size cache_commit_switch,.-cache_commit_switch
.section .note.GNU-stack,"",%progbits
