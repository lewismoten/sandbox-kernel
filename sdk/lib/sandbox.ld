/* SPDX-License-Identifier: MIT; link map for sandbox-kernel ARM32 programs:
   fixed address 0x08000000, a read/execute text segment and a separate
   read/write, never executable data segment on its own pages. */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x08000000;
  .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } :text
  . = ALIGN(4096);
  .data : { *(.data*) } :data
  .bss : { *(.bss*) *(COMMON) } :data
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
