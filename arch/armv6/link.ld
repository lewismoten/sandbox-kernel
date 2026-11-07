ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = LOAD_ADDRESS;
 .text : { KEEP(*(.text.boot)) *(.text*) } :code
 .rodata : { *(.rodata*) } :code
 .data : { *(.data*) } :data
 . = ALIGN(4); __file_end = .;
 .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); __bss_end = .; } :data
 .stacks (NOLOAD) : {
  . += 4096; __irq_stack = .;
  . += 4096; __fiq_stack = .;
  . += 4096; __abort_stack = .;
  . += 4096; __undef_stack = .;
  . += 16384; __svc_stack = .;
 } :data
 __end = .;
 ASSERT(__end < MEMORY_LIMIT, "image exceeds reserved RAM")
 ASSERT(__end <= 0x04000000, "kernel RAM reaches the user page pool (include/uspace.h)")
 /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
}
