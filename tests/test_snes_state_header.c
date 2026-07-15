/* Host unit test for Core/Inc/porting/snes/snes_state_header.h — the savestate
 * stamp snes_SaveState/LoadState (main_snes.c) write and check.
 *
 * main_snes.c is NOT compiled here: it needs external/sm and the emulator. The
 * refusal logic was factored into this header so the test links the REAL
 * predicates — no ROM, no submodule. It pins the two defects that actually
 * shipped from this code (commit 2f4f1343):
 *   - a foreign magic / bumped version is refused (before any payload is read);
 *   - a truncated file is refused, NOT accepted as a half-zeroed machine — the
 *     payload-size check is what closes that hole.
 *
 *   gcc -O2 -Wall -Wextra -std=c11 -ICore/Inc/porting/snes \
 *       tests/test_snes_state_header.c -o /tmp/mtest/test_snes_state_header
 */
#include <stdio.h>
#include "snes_state_header.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

int main(void) {
  const uint32_t PAYLOAD = 1000;            /* what this build streams */
  const long GOOD_FILE = (long)sizeof(snes_state_header_t) + PAYLOAD;

  snes_state_header_t good = { SNES_STATE_MAGIC, SNES_STATE_VERSION, PAYLOAD };

  /* stamp validity (magic + version), checked before a payload byte is read */
  CHECK(snes_state_header_valid(&good), "well-formed stamp is valid");

  snes_state_header_t bad_magic = good; bad_magic.magic = 0xdeadbeefu;
  CHECK(!snes_state_header_valid(&bad_magic), "foreign magic is refused");

  snes_state_header_t bad_ver = good; bad_ver.version = SNES_STATE_VERSION + 1;
  CHECK(!snes_state_header_valid(&bad_ver), "bumped version is refused");

  /* payload/size agreement — the truncation guard */
  CHECK(snes_state_payload_ok(&good, PAYLOAD, GOOD_FILE),
        "matching length + full file accepted");

  /* truncated: header intact, file shorter than header+payload. This is the
   * exact case main_snes.c ACCEPTED before 2f4f1343 (state_read zero-fills past
   * EOF and reported success). The size check must refuse it. */
  CHECK(!snes_state_payload_ok(&good, PAYLOAD, GOOD_FILE - 200),
        "truncated file (short by 200B) is refused");

  /* a header that lies about its length (claims fewer bytes than this build
   * streams) — refused even if the file happens to be that size */
  snes_state_header_t liar = { SNES_STATE_MAGIC, SNES_STATE_VERSION, PAYLOAD - 8 };
  CHECK(!snes_state_payload_ok(&liar, PAYLOAD, GOOD_FILE),
        "header length disagreeing with the measured payload is refused");

  /* a longer-than-expected file (trailing garbage) is refused too */
  CHECK(!snes_state_payload_ok(&good, PAYLOAD, GOOD_FILE + 16),
        "over-long file is refused");

  printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
  return fails ? 1 : 0;
}
