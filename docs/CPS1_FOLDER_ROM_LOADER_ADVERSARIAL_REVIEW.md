# CPS-1 folder ROM loader — adversarial release review

Reviewed branch `explore/cps1-feasibility` at `4ba750bf`, including
`9dd29b56`. This is a release review, not an implementation patch.

## Verdict

**NO-GO. Do not ship these commits as-is.** The device loader rejects the real
`wofj` program chip before Musashi is started, and the device-only flash/XIP path
has never run on hardware. The graphics gather formula itself is well supported;
that does not rescue the loader around it.

## Release blocker found outside the five prompts

### B0 — the reset-vector gate reads the raw program chip in the wrong byte order

`cps1_cache_one_chip_expect()` deliberately calls
`odroid_overlay_cache_file_in_flash(..., false)` and keeps the MAME program chip
verbatim. `cps1_m68k_map_prg_chip()` then base-maps that raw byte stream; on this
little-endian Musashi port, native 16-bit reads supply the word swap MAME denotes
with `ROM_LOAD16_WORD_SWAP`. That part is internally consistent.

The preflight gate is not. `cps1_rom_check_reset_vector()` constructs both longs
byte-by-byte as big-endian. The actual first eight bytes of `tk2j23c.bin` are:

```
ff 00 ee 62 00 00 a2 71
```

The mapped CPU sees SSP `00ff62ee`, PC `000071a2`. The preflight function instead
sees SSP `ff00ee62`, PC `0000a271`; it fails both the WRAM-range and even-PC
checks. `cps1_load_folder_roms()` therefore prints `reset vector rejected` and
returns before `cps1_machine_reset()`.

The host graphics selftest never calls this gate. The existing Musashi selftest
uses a synthetic, already arranged buffer; the real-frame harness uses the old
flat container and also does not exercise this new folder-loader sequence.

**Classification: refuted; release blocker.** A host test must feed a real raw
program chip through the same preflight and two-page-map path used by the device,
then assert the post-reset SSP/PC, before the hardware test even starts.

## 1. CRC32 as the content address

For `n` distinct chip contents and uniformly distributed 32-bit CRCs, the
accidental collision probability is approximately:

```
P(collision) = 1 - exp(-n(n-1) / (2 * 2^32))
```

| unique chips | accidental collision probability |
|---:|---:|
| current generated table: 18 | 0.00000356% |
| 300 | 0.001044% |
| 500 | 0.002905% |
| 1,000 | 0.011629% |
| 3,000 | 0.104684% |
| 5,000 | 0.290557% |
| 9,292 | about 1% |
| 77,163 | about 50% |

Thus collision risk is tiny for the current three WOF sets and still small for
hundreds of CPS-1 chips. The stronger claim in the source and contract — “CRC32
cannot collide with different bytes” — is false. The shared-pool filename check
does not mitigate a collision: different bytes with the same CRC pass the check
by definition, and `find_crc()` then selects the first matching file. File size
does not disambiguate these sets because all supported chips are 512 KiB.

The platform label is not the threshold; the total unique-chip corpus is. A
combined CPS-1/CPS-2/Neo Geo catalog in the low thousands enters the 0.1% regime,
and roughly 9.3k unique chips reaches 1%. Deliberately constructed collisions are
not governed by the birthday probability at all. MAME's own ROM declarations
carry SHA-1 next to CRC32, so the canonical strong identity is already available.

**Classification: refuted as a library-wide content-address design.** CRC32 is a
reasonable fast index for the current tiny table, but not the sole identity for
an expanding or adversarial library. Use a strong digest to confirm a CRC lookup
and to name/disambiguate shared content before widening this scheme.

## 2. `MAX_LIVE_FILES` 8 -> 16

The capacity increase itself is unlikely to change existing SNES/MD/Sega CD/PCE
behavior:

- for a launch holding at most eight distinct addresses, the protected ranges
  and selected write slot are the same;
- only the search-attempt ceiling rises;
- the linked `live_files` object is 128 bytes instead of 64, a 64-byte DTCM BSS
  increase; the reviewed ELF's complete resident BSS is `0x24d8` in a `0x20000`
  DTCM region, so this is not a meaningful RAM-headroom event;
- the existing real-allocator Super Metroid regression test still passes.

There are two holes in the claim that 16 makes CPS-1 safe:

1. The CPS-1 directory scanner can cache `CPS1_MAX_FOLDER_CHIPS == 24` unique
   chip-sized files. After the XIP core blob and 15 chip addresses, `live_add()`
   merely logs that the set is full and returns; the caller still receives an
   unprotected pointer. A folder containing legitimate chips plus 512 KiB junk
   can therefore recreate the erased-live-file class this change is meant to
   close.
2. No test holds the normal CPS-1 set's 11 addresses, forces a ring wrap, writes
   again, and verifies all 11 byte ranges. The current flash test protects one
   ROM while adding one blob; it does not prove the new capacity or the 11-file
   launch shape.

**Classification: survives for other cores; refuted as a complete CPS-1 safety
argument.** The 8->16 edit is not the dangerous part. The un-enforced mismatch
between a 16-entry protector and a 24-file producer is.

## 3. “Host and device go through one accessor”

The narrow graphics-consumer statement survives. All three decoders call
`cps1_rom_gfx_byte()`, and no renderer-side code directly dereferences
`rom->gfx.data`. Flat and chip-backed graphics therefore share the decode logic.

The broader host/device-equivalence statement is false:

- the host real-frame harness attaches a flat assembled image; the device uses
  FatFs, flash-cache XIP pointers, a chip table and two discontiguous program
  mappings;
- `linux/cps1/gfx_chips_selftest.c` reimplements directory scanning and shared
  completion rather than invoking `cps1_load_folder_roms()`; its limit is 16,
  while the device limit is 24;
- the selftest does not exercise FatFs names/sizes, path limits, cache hits,
  cache invalidation, erase boundaries, arbitrary XIP placement, the live range
  list, or program-ROM mapping;
- most decisively, it omits the reset-vector preflight that currently rejects
  the real program chip.

The cross-repository “one table” claim is also weaker than stated. The firmware
and `game-and-what` contain two copied JSON files. They are byte-identical at the
time of review, but the firmware drift gate only compares its local JSON with its
generated C file; it does not compare the library repository's asset. The SHA-256
written in prose is a manual check, not a CI gate.

The folder-layout contract has already drifted: firmware documentation specifies
one `/roms/cps1/.shared/<crc>.bin` pool, while current `game-and-what`
`sd_chip_entries()` emits a complete ten-chip set into every game folder. Its
test explicitly requires two clones to physically duplicate their four shared
chips. Complete folders should still be accepted by the firmware, but the claimed
deduplication and tested release artifact are not the same design.

One more unsafe edge exists in the device-only shared fallback:
`cps1_romset_closest()` does not require a distinguishing chip or even one chip
of overlap. Ties go to the first table entry. If the shared pool contains enough
non-common material, an empty or ambiguous folder can be completed as the wrong
first set instead of being rejected.

**Classification: refuted broadly; survives only at the graphics decoder
accessor boundary.**

## 4. Graphics interleave and slot assignment

For the supported eight-by-512-KiB layout, the address arithmetic matches MAME's
`ROM_LOAD64_WORD` mapping:

```
chip  = half * 4 + (offset-within-half % 8) / 2
index = (offset-within-half / 8) * 2 + (offset-within-half % 2)
```

MAME loads the four 16-bit streams at byte offsets 0, 2, 4 and 6 in each 8-byte
group, with the second four-chip half starting at `0x200000`. The generated WOF,
WOFr1 and WOFJ CRC orders match those declarations.

The strongest local check was rerun during this review with the real WOFJ chips:

- all 4,194,304 gathered bytes equal the independently assembled reference;
- all 4,230 sampled decoder outputs across the 8x8/16x16/32x32 paths match;
- result: PASS.

This is good evidence against both a bad formula and a bad WOFJ slot table. Its
remaining limitations are that only WOFJ real bytes were available, the ROM-based
test politely skips when fixtures are absent, and `tests/run.sh` gates table drift
but does not make this full-byte oracle mandatory for release. A future set with a
different MAME load macro cannot inherit this formula merely because it is CPS-1.

**Classification: survives for the three currently declared WOF-family sets;
judgment unavailable for future layouts.**

## 5. Mandatory pre-release checks, in order

### P0 — must pass before any release candidate

1. Correct and regression-test B0 using the real raw program chip. The test must
   assert preflight SSP/PC and Musashi's SSP/PC after reset through the two
   discontiguous 512-KiB page mappings.
2. On the real device, cold-cache launch a complete WOFJ folder and reach the
   known title/gameplay milestone. Record PC/SSP, a framebuffer hash or screenshot,
   and confirm program/GFX XIP pointers remain stable.
3. Repeat as a warm-cache hit after filling/wrapping the flash ring. Hash all ten
   flash-resident chips before and after the XIP blob and any later cache write.
4. Add a real-allocator test for 11 simultaneous live ranges and a subsequent
   write. Resolve the 24-producer/16-protector mismatch so exceeding protection
   capacity fails the launch rather than only printing a warning.
5. Exercise both intended library artifacts: the actual self-contained ten-chip
   folder and, if `.shared` is still a supported contract, a six-chip clone plus
   four shared files. Test a corrupt/missing shared file and an ambiguous folder.

### P1 — required to call the integration release-safe

6. Make the full-byte graphics oracle non-skipping for the supported release
   fixtures, and test each declared romset or independently verify every slot
   against MAME's CRC+SHA-1 table.
7. Add a cross-repository gate for the library JSON and decide one folder-layout
   contract. At present the JSON matches but the packaging/dedup behavior does not.
8. Smoke-test one large-ROM/core-cache launch each for SNES, MD, Sega CD and PCE
   after the live-range capacity change, including a warm hit followed by the
   core's secondary cache write.
9. Before CPS-2/Neo Geo expansion, retain CRC as an index only and validate content
   with SHA-1/SHA-256. Also test replacement of a same-path file whose FAT mtime is
   unchanged: the global flash-cache key is path+mtime, not file content, so it can
   return a stale XIP copy which the loader then faithfully hashes.

## Evidence rerun

- `cps1-gfx-chips-selftest` with the real WOFJ fixture: PASS, 4 MiB byte sweep and
  4,230 decoder comparisons.
- current `gw_flash_alloc.c` against the fake erase/program flash: PASS for the
  existing Super Metroid one-ROM/one-blob regression.
- `tools/gen_cps1_romset.py --check`: PASS.
- firmware and library JSON SHA-256 at review time:
  `c3d444ba457abaa5a103d38282a4cd782e3751ed78601f67b50611e79fb9e75c`.
- hardware folder-loader execution: not performed by the branch author and no
  device evidence was found during review.

## Final line

**이대로 출하하면 안 된다.** Reset-vector byte-order blocker, 11-live-range
device proof, and a real-device cold/warm launch must be closed first.
