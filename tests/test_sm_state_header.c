/* Host unit tests for Core/Inc/porting/sm/sm_state_header.h — the savestate
 * magic/version/length header sm_system_SaveState/LoadState (main_sm.c) write
 * and check.
 *
 * main_sm.c itself is not compiled here: it pulls in the whole G&W HAL/odroid
 * stack, which this bug does not need and a host build cannot provide. The
 * header check was factored out into sm_state_header.h specifically so this
 * test links the REAL validation logic (sm_state_header_read /
 * sm_state_header_valid), not a hand-copied guess at what main_sm.c does.
 *
 * A savestate is a raw dump of live structs. Move a field in that layout and
 * SM_STATE_VERSION has to go up, or yesterday's file still opens, still reads
 * to the end, and quietly restores nonsense into structs that have since
 * moved. These tests pin: a good header loads, a foreign magic is refused, a
 * foreign version is refused, and a file truncated before the header even
 * finished writing is refused — never a short read silently treated as zero.
 *
 * Compile + run:
 *   gcc -O2 -Wall -Wextra -std=c11 -ICore/Inc/porting/sm \
 *       tests/test_sm_state_header.c -o /tmp/mtest/test_sm_state_header
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sm_state_header.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

/* Writes raw bytes to a fresh tmpfile and rewinds it, so sm_state_header_read
 * sees exactly what a LoadState() call would see reading from the top of a
 * file on disk. */
static FILE *file_of(const void *data, size_t n) {
  FILE *f = tmpfile();
  if (!f) { perror("tmpfile"); exit(1); }
  if (n && fwrite(data, 1, n, f) != n) { perror("fwrite"); exit(1); }
  rewind(f);
  return f;
}

static void test_good_header_loads(void) {
  sm_state_header_t hdr = { SM_STATE_MAGIC, SM_STATE_VERSION, 12345 };
  FILE *f = file_of(&hdr, sizeof(hdr));

  sm_state_header_t out = {0, 0, 0};
  bool ok = sm_state_header_read(f, &out);
  fclose(f);

  CHECK(ok, "this build's own header is accepted");
  CHECK(out.bytes == 12345, "payload length field round-trips");
}

static void test_foreign_magic_refused(void) {
  sm_state_header_t hdr = { 0xdeadbeefu, SM_STATE_VERSION, 100 };
  FILE *f = file_of(&hdr, sizeof(hdr));

  sm_state_header_t out = {0, 0, 0};
  bool ok = sm_state_header_read(f, &out);
  fclose(f);

  CHECK(!ok, "wrong magic (not this core's file at all) is refused");
  CHECK(out.magic == 0xdeadbeefu, "the raw foreign magic is still handed back for logging");
}

static void test_foreign_version_refused(void) {
  /* Same magic, layout has moved on: exactly what happens when a field gets
   * added to a struct ppu_saveload/RtlSaveLoadState streams, and the build
   * that wrote this file predates the bump. */
  sm_state_header_t hdr = { SM_STATE_MAGIC, SM_STATE_VERSION + 1, 100 };
  FILE *f = file_of(&hdr, sizeof(hdr));

  sm_state_header_t out = {0, 0, 0};
  bool ok = sm_state_header_read(f, &out);
  fclose(f);

  CHECK(!ok, "right magic, wrong version (an older/newer build's layout) is refused");
}

static void test_truncated_file_refused(void) {
  /* Fewer bytes than sizeof(sm_state_header_t) on disk — a write that got cut
   * off, or a file truncated in transit. fread() must come up short, and
   * sm_state_header_read must say so rather than reading uninitialised/garbage
   * bytes as a magic. */
  sm_state_header_t hdr = { SM_STATE_MAGIC, SM_STATE_VERSION, 100 };
  FILE *f = file_of(&hdr, sizeof(hdr) - 3);

  sm_state_header_t out = { 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau };  /* poison */
  bool ok = sm_state_header_read(f, &out);
  fclose(f);

  CHECK(!ok, "a file shorter than the header itself is refused");
}

static void test_empty_file_refused(void) {
  FILE *f = file_of(NULL, 0);

  sm_state_header_t out = {0, 0, 0};
  bool ok = sm_state_header_read(f, &out);
  fclose(f);

  CHECK(!ok, "a zero-length file is refused, not read as an all-zero header");
}

int main(void) {
  test_good_header_loads();
  test_foreign_magic_refused();
  test_foreign_version_refused();
  test_truncated_file_refused();
  test_empty_file_refused();

  printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
  return fails ? 1 : 0;
}
