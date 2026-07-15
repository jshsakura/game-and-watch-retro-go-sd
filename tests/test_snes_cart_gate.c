/* Host unit test for Core/Inc/porting/snes/snes_cart_header.h — the gate that
 * decides, before a ROM is handed to the core, whether it needs a coprocessor
 * this core cannot run (SA-1/SuperFX/DSP/Cx4/S-DD1). Get it wrong and the game
 * hard-faults mid-boot instead of returning to the launcher with a message.
 *
 * main_snes.c is NOT compiled here: it needs external/sm (absent in CI's
 * host-tests job) and the whole emulator. The gate was factored into the header
 * precisely so this links the REAL logic, not a hand-copied guess — no ROM and
 * no submodule required.
 *
 * Cart header layout (found at 0x7fb0 LoROM / 0xffb0 HiROM): +0x26 = ROM type
 * ($ffd6), +0x2c/2d = checksum complement, +0x2e/2f = checksum. A header only
 * "counts" when checksum ^ complement == 0xffff.
 *
 *   gcc -O2 -Wall -Wextra -std=c11 -ICore/Inc/porting/snes \
 *       tests/test_snes_cart_gate.c -o /tmp/mtest/test_snes_cart_gate
 */
#include <stdio.h>
#include <string.h>
#include "snes_cart_header.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

/* Build a ROM of `len` bytes with a header at `off` carrying ROM type `type`
 * and a checksum pair that validates (or not, when `valid` is false). */
static void put_header(uint8_t *rom, uint32_t off, uint8_t type, int valid) {
  uint8_t *h = rom + off;
  h[0x26] = type;
  uint16_t cks = 0x1234;
  uint16_t icks = valid ? (uint16_t)(cks ^ 0xffff) : (uint16_t)(cks ^ 0x0f0f);
  h[0x2c] = icks & 0xff; h[0x2d] = icks >> 8;
  h[0x2e] = cks & 0xff;  h[0x2f] = cks >> 8;
}

int main(void) {
  static uint8_t rom[0x20000];   /* big enough for a HiROM header at 0xffb0 */

  /* --- LoROM (header at 0x7fb0) --- */
  memset(rom, 0, sizeof rom);
  put_header(rom, 0x7fb0, 0x00, 1);   /* plain ROM */
  CHECK(!snes_cart_needs_coprocessor(rom, sizeof rom), "LoROM type 0 (ROM) accepted");

  memset(rom, 0, sizeof rom);
  put_header(rom, 0x7fb0, 0x02, 1);   /* ROM+RAM+battery */
  CHECK(!snes_cart_needs_coprocessor(rom, sizeof rom), "LoROM type 2 (RAM+batt) accepted");

  memset(rom, 0, sizeof rom);
  put_header(rom, 0x7fb0, 0x03, 1);   /* first coprocessor type */
  CHECK(snes_cart_needs_coprocessor(rom, sizeof rom), "LoROM type 3 (coprocessor) rejected");

  memset(rom, 0, sizeof rom);
  put_header(rom, 0x7fb0, 0x05, 1);   /* DSP-ish */
  CHECK(snes_cart_needs_coprocessor(rom, sizeof rom), "LoROM type 5 (coprocessor) rejected");

  /* --- HiROM (header at 0xffb0) --- */
  memset(rom, 0, sizeof rom);
  put_header(rom, 0xffb0, 0x03, 1);
  CHECK(snes_cart_needs_coprocessor(rom, sizeof rom), "HiROM type 3 (coprocessor) rejected");

  memset(rom, 0, sizeof rom);
  put_header(rom, 0xffb0, 0x01, 1);
  CHECK(!snes_cart_needs_coprocessor(rom, sizeof rom), "HiROM type 1 (ROM+RAM) accepted");

  /* --- an INVALID-checksum header must not decide anything: a coprocessor byte
   *     sitting behind a bad checksum is a bad dump, not a reject-worthy fact.
   *     The gate returns false and lets snes_loadRom sort it out. --- */
  memset(rom, 0, sizeof rom);
  put_header(rom, 0x7fb0, 0x05, 0);   /* coprocessor type, but checksum fails */
  CHECK(!snes_cart_needs_coprocessor(rom, sizeof rom),
        "invalid-checksum header does not false-trigger a reject");

  /* --- a ROM too short to even hold a header: no crash, no reject --- */
  CHECK(!snes_cart_needs_coprocessor(rom, 0x100),
        "sub-header-length ROM handled without reading past the buffer");

  printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
  return fails ? 1 : 0;
}
