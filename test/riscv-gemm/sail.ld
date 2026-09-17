/* Bare-metal layout for the sail-riscv model: RAM at 0x80000000, entry first. */
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text.start : { *(.text.start) }
  .text   : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) *(.sdata*) }
  .bss    : { *(.bss*) *(.sbss*) *(COMMON) }
  .tohost ALIGN(64) : { *(.tohost) }
}
