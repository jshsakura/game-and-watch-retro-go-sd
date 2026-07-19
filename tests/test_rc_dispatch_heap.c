/* test_rc_dispatch_heap.c — proves rc_dispatch_init() allocates ZERO heap.
 *
 * The old rc_dispatch.c (pre-fix) called malloc() once per non-empty bank to
 * build per-bank open-addressing hash tables (~85 KB for SMW's 7 banks). The
 * DTCM heap is only 81 KB and shared with the launcher. rc activated
 * successfully (code-region hash gate PASSED) then OOM-crashed on the device
 * immediately. This class of bug is invisible to docker link (no heap model)
 * and to the QEMU rig (which uses its own dispatch tail).
 *
 * This test compiles the REAL rc_dispatch.c (whole-file #include, not a copy)
 * with malloc/calloc/realloc/free redirected to counters. If ANY heap
 * allocation fires during init, the test fails.
 *
 * GREEN state (fixed code): the counter stays at 0 — caller-provided overlay
 *   BSS + binary search, no heap.
 * RED state (old code): the counter would be >0 and this test fails.
 *
 * Build: gcc -O2 -Wall -Wextra -std=c11 -Iexternal/sm/src/snes \
 *          tests/test_rc_dispatch_heap.c -o /tmp/.../test_rc_dispatch_heap
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Include the REAL cpu.h so Cpu is defined for the dispatch signatures.
 * Pulled in via -Iexternal/sm/src/snes. */
#include "cpu.h"

/* ---- heap allocation counter ------------------------------------------- */
static int g_alloc_count = 0;

/* These are the RED-detection hooks: if anyone adds a heap call back into
 * rc_dispatch.c, the macro redirect below routes it here and the counter
 * goes non-zero. When the code is correct (GREEN) they are never called, so
 * the unused-function warning is expected and suppressed. */
__attribute__((unused))
static void *test_malloc(size_t n)  { g_alloc_count++; return malloc(n); }
__attribute__((unused))
static void *test_calloc(size_t n, size_t sz) { g_alloc_count++; return calloc(n, sz); }
__attribute__((unused))
static void *test_realloc(void *p, size_t n)  { g_alloc_count++; return realloc(p, n); }
__attribute__((unused))
static void  test_free(void *p)     { free(p); }

/* Redirect every heap allocator inside rc_dispatch.c to the counters. Must
 * be defined BEFORE the whole-file #include so the macro catches any call.
 * The system headers above are already parsed, so their #include guards make
 * the re-include inside rc_dispatch.c a no-op — the macro never touches a
 * declaration, only call sites. */
#define malloc  test_malloc
#define calloc  test_calloc
#define realloc test_realloc
#define free    test_free

#include "rc_dispatch.c"   /* the REAL file — not a stub, not a copy */

#undef malloc
#undef calloc
#undef realloc
#undef free

/* ---- test data: 16 sites across 2 banks -------------------------------- */
#define N_SITES 16
#define N_BANK0 8
#define N_BANK1 8

/* 24-bit kpc = (bank << 16) | pc. Two banks, 8 sites each, unsorted to
 * exercise the sort pass. */
static const uint32_t test_addrs[N_SITES] = {
  /* bank 0x00 — PCs in non-sorted order */
  0x008050, 0x008000, 0x008100, 0x008010, 0x008040, 0x008020, 0x008005, 0x008030,
  /* bank 0x01 */
  0x019050, 0x019000, 0x019100, 0x019010, 0x019040, 0x019020, 0x019005, 0x019030,
};

/* Dummy site functions — never called, just need the right type. */
static void site_fn_00(Cpu *cpu) { (void)cpu; }
static void (*test_fns[N_SITES])(Cpu *) = {
  site_fn_00, site_fn_00, site_fn_00, site_fn_00,
  site_fn_00, site_fn_00, site_fn_00, site_fn_00,
  site_fn_00, site_fn_00, site_fn_00, site_fn_00,
  site_fn_00, site_fn_00, site_fn_00, site_fn_00,
};

/* Caller-provided buffers (overlay BSS on device, stack/static here). */
static rc_entry_t test_table[N_SITES];
static uint32_t   test_bank_off[256];
static uint32_t   test_bank_cnt[256];

int failures = 0;

static void check(int cond, const char *msg) {
  printf("  %s %s\n", cond ? "OK  " : "FAIL", msg);
  if (!cond) failures++;
}

int main(void) {
  printf("=== rc_dispatch heap-allocation test ===\n");

  /* 1. Init with caller-provided buffers — NO heap allocation allowed. */
  g_alloc_count = 0;
  rc_dispatch_init(test_table, test_bank_off, test_bank_cnt,
                   test_addrs, N_SITES, test_fns);
  check(g_alloc_count == 0, "zero heap allocations during rc_dispatch_init");
  if (g_alloc_count > 0)
    printf("       expected 0 allocations, got %d\n", g_alloc_count);

  /* 2. g_rc_active must be true after init. */
  check(g_rc_active == true, "g_rc_active is true after init");

  /* 3. Every site must be findable by (bank, pc) → correct 1-based id. */
  int lookup_ok = 1;
  for (int i = 0; i < N_SITES; i++) {
    uint8_t  bank = test_addrs[i] >> 16;
    uint16_t pc   = test_addrs[i] & 0xffff;
    uint16_t id   = rc_dispatch_lookup(bank, pc);
    if (id != (uint16_t)(i + 1)) {
      printf("       site %d: lookup(bank=0x%02x, pc=0x%04x) returned id=%d, expected %d\n",
             i, bank, pc, id, i + 1);
      lookup_ok = 0;
    }
  }
  check(lookup_ok, "all 16 sites found with correct 1-based ids");

  /* 4. Misses must return 0 (interpreter fallback). */
  check(rc_dispatch_lookup(0x00, 0x8077) == 0, "miss in bank 0 returns 0");
  check(rc_dispatch_lookup(0x01, 0x9077) == 0, "miss in bank 1 returns 0");
  check(rc_dispatch_lookup(0x02, 0x8000) == 0, "miss in empty bank returns 0");

  /* 5. Reset clears g_rc_active (and must NOT allocate either). */
  g_alloc_count = 0;
  rc_dispatch_reset();
  check(g_alloc_count == 0, "zero heap allocations during rc_dispatch_reset");
  check(g_rc_active == false, "g_rc_active is false after reset");

  if (failures == 0)
    printf("\nALL PASS — rc_dispatch_init/reset allocate zero heap, lookup correct\n");
  else
    printf("\n%d FAILURE(S)\n", failures);
  return failures ? 1 : 0;
}
