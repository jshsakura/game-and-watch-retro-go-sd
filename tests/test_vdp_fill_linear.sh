#!/bin/bash
# The 32X VDP autofill in picodrive-32x-vdp-fill-linear.patch against the
# original loop, for every start, several lines and every length up to 4 lines.
# It compiles gnw_vdp_fill() itself, cut out of the patched memory.c.
# PR #65 first shipped a version that only wrote the wrapped tail (m & 0xff), so
# a fill of more than rem + 256 halfwords left cells unwritten.
# usage: test_vdp_fill_linear.sh   (PD_MEMORY_C=<pico/32x/memory.c> to override)
set -eu
cd "$(dirname "$0")/.."
SRC=${PD_MEMORY_C:-external/picodrive/pico/32x/memory.c}
PATCH=patches/picodrive-32x-vdp-fill-linear.patch
[ -f "$SRC" ] || { echo "SKIPPED: $SRC not present (picodrive submodule not fetched)"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/pico/32x"; cp "$SRC" "$T/pico/32x/memory.c"
if grep -q 'GNW_FILL_BEGIN' "$T/pico/32x/memory.c"; then :
elif patch -d "$T" -p1 --dry-run -s < "$PATCH" >/dev/null 2>&1; then patch -d "$T" -p1 -s < "$PATCH" >/dev/null
else echo "FAIL: $PATCH neither applies to nor is in $SRC"; exit 1; fi
awk '/GNW_FILL_BEGIN/{on=1} on{print} /GNW_FILL_END/{on=0}' "$T/pico/32x/memory.c" > "$T/fill.inc"
[ -s "$T/fill.inc" ] || { echo "FAIL: gnw_vdp_fill markers not found"; exit 1; }
cat > "$T/t.c" <<'C'
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t u16; typedef uint32_t u32;
#include "fill.inc"
/* the original walk */
static void ref_fill(u16 *dram, u32 a, u32 len, u16 d) {
  while (len--) { dram[a] = d; a = (a & 0xff00) | ((a + 1) & 0xff); }
}
/* the version PR #65 first shipped: tail written as m & 0xff */
static void first_pr_fill(u16 *dram, u32 a, u32 len, u16 d) {
  u32 lo = a & 0xff, rem = 0x100 - lo, i;
  if (len < rem) { for (i = 0; i < len; i++) dram[(a & 0xff00) | (lo + i)] = d; }
  else { u32 m = len - rem;
    for (i = lo; i < 0x100; i++) dram[(a & 0xff00) | i] = d;
    for (i = 0; i < (m & 0xff); i++) dram[(a & 0xff00) | i] = d; }
}
static u16 A[0x10000], B[0x10000];
int main(void) {
  static const u32 his[] = { 0x0000, 0x0100, 0x5a00, 0xff00 };
  long cases = 0, bad = 0, first_pr_bad = 0;
  for (unsigned h = 0; h < sizeof his / sizeof *his; h++)
    for (u32 lo = 0; lo < 256; lo++)
      for (u32 len = 1; len <= 1024; len++) {
        u32 a = his[h] | lo, w0 = his[h] >= 0x100 ? his[h] - 0x100 : 0, w1 = his[h] + 0x200 > 0x10000 ? 0x10000 : his[h] + 0x200;
        for (u32 i = w0; i < w1; i++) A[i] = B[i] = (u16)(0x1111 + i);
        ref_fill(A, a, len, 0xbeef); gnw_vdp_fill(B, a, len, 0xbeef);
        if (memcmp(A + w0, B + w0, (w1 - w0) * 2)) { if (bad++ < 5) printf("FAIL hi %04x lo %u len %u\n", his[h], lo, len); }
        for (u32 i = w0; i < w1; i++) B[i] = (u16)(0x1111 + i);
        first_pr_fill(B, a, len, 0xbeef);
        if (memcmp(A + w0, B + w0, (w1 - w0) * 2)) first_pr_bad++;
        cases++;
      }
  printf("vdp_fill_linear: %ld cases, %ld failures (the first PR version differed in %ld, so the test can see this bug class)\n", cases, bad, first_pr_bad);
  if (!first_pr_bad) { printf("FAIL: the test cannot tell the first PR version apart\n"); return 1; }
  return bad ? 1 : 0;
}
C
gcc -O2 -Wall -Wextra -std=gnu11 -I"$T" "$T/t.c" -o "$T/t" && "$T/t"
