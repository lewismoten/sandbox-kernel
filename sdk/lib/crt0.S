/* SPDX-License-Identifier: MIT; sandbox-kernel SDK program entry (ARM
   state). r0 holds the read-only sbx_startup record; main's return value is
   the exit status: returning to the kernel-supplied lr ends the program. */
.syntax unified
.arm
.section .text.start,"ax"
.global _start
_start:
    push {r4, lr}
    bl main
    pop {r4, pc}
.section .note.GNU-stack,"",%progbits
