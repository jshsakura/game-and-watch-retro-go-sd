/* QEMU mps2-an500 (Cortex-M7) — WonderSwan variant.
 * WS ROMs reach 8-16MB and are mapped XIP-style (read-only) on the device, so
 * the ROM blob gets its own section at the front of the 16MB PSRAM instead of
 * being copied into the 4MB RAM like small cores' ROMs. Heap floats after it.
 *   CODE  4MB @ 0x00000000 — vectors, text, rodata
 *   RAM   4MB @ 0x20000000 — data, bss (core static arrays)
 *   BIG  16MB @ 0x60000000 — ROM blob (.rom), then heap, then stack
 * No caches, no wait states in QEMU: this rig counts instructions, not time. */
ENTRY(Reset_Handler)

MEMORY
{
  CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
  BIG  (rwx) : ORIGIN = 0x60000000, LENGTH = 32M
}

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > CODE
  /* .itcm_sh2bus: the six SH-2 guest-bus entry points carry their own section
     name under GNW_32X_CORE so the device script can place them in ITCM next
     to the interpreter (see pico/32x/memory.c). The rig has no TCM to model,
     but it must still claim the section or those functions become an orphan. */
  .text : { *(.text*) *(.itcm_sh2bus) *(.glue_7) *(.glue_7t) } > CODE
  .rodata : { *(.rodata*) . = ALIGN(4); } > CODE
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > CODE

  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> CODE
  .bss : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM

  /* ROM blob (objcopy binary, renamed .data -> .rom): loaded, read-only, XIP. */
  .rom : { . = ALIGN(4); KEEP(*(.rom)) . = ALIGN(4); } > BIG

  .heap (NOLOAD) : { . = ALIGN(8); _sheap = .; . += 8M; _eheap = .; } > BIG
  .stack (NOLOAD) : { . = ALIGN(8); . += 512K; _estack = .; } > BIG
}
