/* QEMU mps2-an500 (Cortex-M7), SNES variant of mps2_an500.ld.
 * Difference: the ROM blob (up to 6 MB) goes in PSRAM (.rom_blob), not .data —
 * a 4 MB cart would otherwise overflow the 4 MB RAM when .data is copied at boot.
 * The core memcpy's it into a heap buffer anyway, so it never needs fast RAM.
 *   CODE  4MB  @ 0x00000000  — vectors, text, rodata
 *   RAM   4MB  @ 0x20000000  — data, bss
 *   BIG  16MB  @ 0x60000000  — ROM blob, then heap (core arenas) and stack
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
  /* .itcm_snes_interp*: the Thumb-2 65816 and SPC700 engines, which the device
   * links into ITCM. The rig has no ITCM; keeping them in .text stops them
   * landing wherever ld puts an orphan section. Matches nothing without
   * run_snes_t2.sh. */
  .text : { *(.text*) *(.itcm_snes_interp*) *(.glue_7) *(.glue_7t) } > CODE
  .rodata : { *(.rodata*) . = ALIGN(4); } > CODE
  .ARM.exidx : { *(.ARM.exidx*) } > CODE

  /* ROM blob (objcopy --rename-section .data=.rom_blob): placed in PSRAM,
   * loaded there directly, never copied to RAM. */
  .rom_blob : { . = ALIGN(4); *(.rom_blob*) . = ALIGN(4); } > BIG

  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> CODE
  .bss : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM

  .heap (NOLOAD) : { . = ALIGN(8); _sheap = .; . += 4M; _eheap = .; } > BIG
  .stack (NOLOAD) : { . = ALIGN(8); . += 1M; _estack = .; } > BIG
}
