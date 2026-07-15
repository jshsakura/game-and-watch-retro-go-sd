#pragma once
/* SNES cart-header inspection — the one bit the launcher must get right BEFORE
 * it hands a ROM to the core: is this a plain LoROM/HiROM cart, or does it need
 * a coprocessor (SA-1, SuperFX, DSP-x, Cx4, S-DD1, …) this core cannot run?
 *
 * Gate it at load with a message, or the game hard-faults mid-boot when the CPU
 * reads a chip register that answers with open bus. Pulled out of main_snes.c
 * so a host test can drive snes_cart_needs_coprocessor() on synthetic headers
 * without linking the emulator core — see tests/test_snes_cart_gate.c. */
#include <stdint.h>
#include <stdbool.h>

/* $ffd6 (ROM type): 0=ROM 1=ROM+RAM 2=ROM+RAM+battery; 3+ = coprocessor.
 * Find the header the same way the loader scores it: the offset whose
 * checksum ^ complement is 0xFFFF wins; if neither validates, return false and
 * let snes_loadRom decide (a bad dump is a different failure). */
static inline bool snes_cart_needs_coprocessor(const uint8_t *rom, uint32_t len) {
  static const uint32_t offs[2] = { 0x7fb0, 0xffb0 };   /* LoROM, HiROM */
  for (int i = 0; i < 2; i++) {
    if (offs[i] + 0x30 > len) continue;
    const uint8_t *h = rom + offs[i];
    uint16_t cks  = h[0x2e] | (h[0x2f] << 8);
    uint16_t icks = h[0x2c] | (h[0x2d] << 8);
    if ((cks ^ icks) == 0xffff)
      return h[0x26] >= 0x03;    /* $ffd6 = header+0x26 */
  }
  return false;
}
