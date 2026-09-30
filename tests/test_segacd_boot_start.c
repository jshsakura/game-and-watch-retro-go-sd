/* Compiles the real Core/Inc/porting/segacd/segacd_boot_start.h -- the rule
 * the firmware loop and the host rig both run -- and pins its contract:
 * START is pulsed only while the main 68K is inside the BIOS ROM window, and
 * never again once it has run anywhere else. */
#include <stdio.h>
#include <stdlib.h>

#include "porting/segacd/segacd_boot_start.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL %s\n", msg); failures++; } } while (0)

static int presses_in(segacd_boot_start_t *s, uint32_t pc, int frames)
{
  int n = 0;
  for (int i = 0; i < frames; i++)
    n += segacd_boot_start_pad(s, pc) != 0;
  return n;
}

int main(void)
{
  segacd_boot_start_t s;

  /* Arrange: fresh boot, main 68K in the BIOS menu loop. */
  segacd_boot_start_init(&s, true);
  /* Act/Assert: nothing during the settle delay, then HOLD of every PERIOD. */
  CHECK(presses_in(&s, 0x1c26, SEGACD_BOOT_START_DELAY) == 0,
        "START pressed before the BIOS had drawn its menu");
  CHECK(presses_in(&s, 0x1c26, SEGACD_BOOT_START_PERIOD * 4) ==
            (int)(SEGACD_BOOT_START_HOLD * 4),
        "START not pulsed HOLD frames per PERIOD in the BIOS window");

  /* The game's code runs from main RAM: the helper retires for good... */
  CHECK(segacd_boot_start_pad(&s, 0xff0000) == 0, "START pressed in game code");
  CHECK(!s.active, "helper still active after the main CPU left the BIOS");
  /* ...so a later BIOS call is not mistaken for the menu. */
  CHECK(presses_in(&s, 0x0210, SEGACD_BOOT_START_PERIOD * 4) == 0,
        "START pressed again when the game called back into the BIOS");

  /* Word RAM (0x200000+) is outside the window too, and the address is
   * masked to the 68K's 24 bits (FAME can report 0xffffxxxx). */
  segacd_boot_start_init(&s, true);
  CHECK(segacd_boot_start_pad(&s, 0x23d1f2) == 0 && !s.active,
        "word-RAM execution not treated as leaving the BIOS");
  segacd_boot_start_init(&s, true);
  CHECK(segacd_boot_start_pad(&s, 0xffffe868u) == 0 && !s.active,
        "sign-extended RAM PC not treated as leaving the BIOS");
  segacd_boot_start_init(&s, true);
  (void)segacd_boot_start_pad(&s, 0xff01fffeu);   /* 24-bit 0x01fffe: BIOS */
  CHECK(s.active, "high byte not masked off before the window test");

  /* A state load starts past the menu: never press. */
  segacd_boot_start_init(&s, false);
  CHECK(presses_in(&s, 0x1c26, 600) == 0, "START pressed after a state load");

  if (failures) return 1;
  printf("  OK   segacd_boot_start: presses only inside the BIOS, retires for good\n");
  return 0;
}
