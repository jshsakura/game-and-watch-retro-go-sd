/* QEMU mps2-an500 (Cortex-M7):
 *   CODE  4MB  @ 0x00000000  — ZBT SSRAM1: vectors, text, rodata, the ROM blob
 *   RAM   4MB  @ 0x20000000  — ZBT SSRAM2/3: data, bss
 *   BIG  16MB  @ 0x60000000  — PSRAM: heap (core arenas) and stack
 * No caches, no wait states in QEMU: this rig counts instructions, not time. */
ENTRY(Reset_Handler)

MEMORY
{
  CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
  BIG  (rwx) : ORIGIN = 0x60000000, LENGTH = 16M
}

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > CODE
  .text : { *(.text*) *(.glue_7) *(.glue_7t) } > CODE
  .rodata : { *(.rodata*) . = ALIGN(4); } > CODE
  .ARM.exidx : { *(.ARM.exidx*) } > CODE

  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> CODE
  .bss : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM

  .heap (NOLOAD) : { . = ALIGN(8); _sheap = .; . += 14M; _eheap = .; } > BIG
  .stack (NOLOAD) : { . = ALIGN(8); . += 1M; _estack = .; } > BIG
}
