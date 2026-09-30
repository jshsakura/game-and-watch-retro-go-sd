#pragma once

/* The Japanese and European Mega-CD BIOS stop at their menu with a disc in
 * and wait for START; the US BIOS only gives up and boots after a long CD
 * player detour. Either way the player sees a BIOS screen instead of the game.
 *
 * Pulse START while the main 68K is still inside the BIOS ROM window, and
 * stop for good the first time it runs anywhere else: from then on the game
 * owns the pad, and a BIOS call it makes later must not be mistaken for the
 * menu. Shared by the firmware and tools/pico_host_segacd so the host proves
 * the rule the device runs. */

#include <stdbool.h>
#include <stdint.h>

#define SEGACD_BOOT_BIOS_END     0x20000u /* main 68K BIOS ROM: 0x000000-0x01ffff */
#define SEGACD_BOOT_START_DELAY  120u     /* let the BIOS draw its menu first */
#define SEGACD_BOOT_START_PERIOD 60u
#define SEGACD_BOOT_START_HOLD   6u
#define SEGACD_BOOT_PAD_START    (1u << 7)

typedef struct {
  bool active;
  uint32_t frame;
} segacd_boot_start_t;

static inline void segacd_boot_start_init(segacd_boot_start_t *s, bool active)
{
  s->active = active;
  s->frame = 0;
}

/* Returns the START bit to OR into the pad for this frame. */
static inline uint16_t segacd_boot_start_pad(segacd_boot_start_t *s, uint32_t main_pc)
{
  if (!s->active) return 0;
  if ((main_pc & 0xffffffu) >= SEGACD_BOOT_BIOS_END) {
    s->active = false;
    return 0;
  }
  uint32_t f = s->frame++;
  if (f < SEGACD_BOOT_START_DELAY) return 0;
  return (f % SEGACD_BOOT_START_PERIOD) < SEGACD_BOOT_START_HOLD ? SEGACD_BOOT_PAD_START : 0;
}
