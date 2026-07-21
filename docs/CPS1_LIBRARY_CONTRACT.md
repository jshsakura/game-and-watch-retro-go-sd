# CPS-1 library layout — the contract between the library app and the device

Two programs have to agree on this: **game-and-what** (the library/upload side,
which prepares the SD card) and **retro-go-sd** (the firmware, which reads it).
This file is the agreement. Neither side may change the layout without the
other.

Status: firmware side implemented (`Core/Src/porting/cps1/main_cps1.c`,
`cps1_romset.c`), verified by `linux/build/cps1-gfx-chips-selftest`.

---

## 1. Layout

```
/roms/cps1/
    <Game Name>/            ← ONE FOLDER PER GAME. This is the launcher entry.
        tk2j23c.bin             raw MAME chip dumps, ORIGINAL filenames
        tk2j22c.bin
        tk205.bin … tk208.bin
    .shared/                ← chips common to several sets, stored ONCE,
        0d9cb9bf.bin            named by their CRC32 (see §2.1)
        45227027.bin
        c5ca2460.bin
        e349551c.bin
```

**A parent set is not a game folder.** If the user owns both the World and the
Japanese release as playable entries, each gets its own `<Game Name>/` folder
and the chips they have in common live once in `.shared/`. If only the clone is
owned, there is no parent folder at all — just the clone's folder plus the
shared chips it needs.

- **No archives.** The device has no inflate in the emulator path, and more to
  the point it has nowhere to put 4 MB of decompressed graphics: RAM_EMU is
  724 KB. Chips are cached into external flash and read in place (XIP), which
  only works on a file that is already the raw chip bytes.
- **No container, no sidecar, no index file.** The `.cps1` container this
  project once invented is retired (`0bd923c1`); MAME's chip interleave is pure
  address arithmetic, so nothing needs assembling.
- **Files are byte-for-byte the MAME dumps.** No byte-swapping, no renaming,
  no padding. MAME applies `ROM_REVERSE` when it builds its big-endian program
  region, and a little-endian `*(uint16*)` read of the raw chip undoes exactly
  that, so the two cancel: verbatim is correct. (Proof: `tk2j23c.bin` begins
  `ff 00 ee 62 00 00 a2 71`, which reads back as SSP=`0x00FF62EE`,
  PC=`0x000071A2` — a stack pointer inside work RAM and an even PC.)
- **`.shared/` is invisible to the launcher.** Its folder scan skips any name
  beginning with `.` (`rg_emulators.c`), so it never lists as a game.

## 2. `.shared/` — why the parent set is not duplicated

A MAME clone archive contains only what is unique to it. `wofj.zip` holds its
two program chips and the four *upper* graphics chips; the four *lower* ones
are byte-identical to the parent's and are simply absent.

Making every game folder self-contained would store those shared chips once per
clone — twice on the card, and **twice in the flash cache**, which keys on path
rather than content and therefore cannot tell that two paths hold identical
bytes. `.shared/` stores them once and every set that needs them finds them
there.

### 2.1 Shared chips are named by CRC32, and only there

Inside a game folder, filenames are the original MAME ones and are ignored.
Inside `.shared/`, the filename **is** the CRC32, lowercase hex, `.bin`:
`0d9cb9bf.bin`. Two independent reasons, either one fatal to the obvious
"just keep the original names" design:

1. **Original MAME names collide across sets.** They are unique only within a
   game family. Every Street Fighter II revision ships chips called
   `s92_*.rom` whose *contents differ*. A flat pool of original names
   overwrites one game's chip with another's, and the result is a game that
   loads and renders wrongly. A CRC32 cannot collide with different bytes —
   it is already the identity this whole system keys on, so using it as the
   filename just says so out loud.
2. **A pool that must be scanned is a pool every launch pays for.** With ten
   games' parents in it, launching one game would hash and flash-cache ~40 MB
   belonging to nine others. Naming by CRC turns the lookup into a direct open
   of exactly the chips this romset lacks — no scan, nothing cached that the
   game does not use.

The name is **checked, not trusted**: a chip fetched as `<crc>.bin` is hashed
like any other and rejected if it does not hash to its own name.

### 2.2 Load order

1. Scan the game folder; cache and hash every 512 KB file found.
2. If that already completes a romset, stop. `.shared/` is never opened.
3. Otherwise take the closest set and open `.shared/<crc>.bin` for each of its
   missing chips, and only those.
4. Re-match. Still incomplete → the on-screen error in §4.

## 3. Chip identification is by CRC32. Never by filename.

This is the part that fails silently if it is got wrong, so it is the part
both sides must implement identically.

A romset's `ROM_LOAD` order is **not** its filename order:

| file | CRC32 | GFX slot |
|---|---|---|
| `tk2_gfx1.rom` | `0d9cb9bf` | 0 |
| `tk2_gfx3.rom` | `45227027` | **1** |
| `tk2_gfx2.rom` | `c5ca2460` | **2** |
| `tk2_gfx4.rom` | `e349551c` | 3 |

and in the Japanese set the upper four chips are named `tk205…tk208`, which
sort *ahead* of `tk2_gfx1…4` entirely. Assign slots in filename order and every
file loads, every size checks out, and the graphics are wrong with nothing
reporting an error.

Measured, not asserted: the mutation test that assigns slots by filename makes
**3,584,744 of 4,194,304 bytes (85 %)** differ from the reference image.

### 3.1 One table, generated — not two kept in step by hand

```
tools/cps1_romsets.json          ← THE source of truth, in BOTH repos, byte-identical
  │                                sha256 c3d444ba457abaa5a103d38282a4cd782e3751ed78601f67b50611e79fb9e75c
  ├─ retro-go-sd:   tools/gen_cps1_romset.py → Core/Src/porting/cps1/cps1_romset.c
  └─ game-and-what: read directly for upload validation
```

`cps1_romset.c` says GENERATED FILE at the top and is not hand-edited.
`tests/run.sh` runs `gen_cps1_romset.py --check`, which fails on **both**
drift directions — an edit to the `.c`, and a JSON change nobody regenerated.
Verified by breaking each on purpose.

Record the sha256 above when the JSON changes, so a repo holding a stale copy
is detectable from its own side without consulting the other.

The library's older `tools/cps1_rom_pack.py` `ROMSETS` dict holds the same
data, but that script is **retired** (it built the abandoned `.cps1`
container). Read the JSON, not the dict.

Note also that `wof.zip` as distributed actually contains the **wofr1** romset
— its program CRCs are `11fb2ed1`/`479b3f24`, not `wof`'s. A zip's name does
not describe its contents. One more reason to key on hashes.

## 4. What each side owns

### game-and-what (library / upload)

1. Accept a MAME `.zip` upload **or** a folder.
2. Extract it. Never store the zip as the playable artifact.
3. Identify every member by CRC32 against the romset table.
4. Write the game's own chips to `/roms/cps1/<Game Name>/` under their original
   names, and any chip that more than one set uses to
   `/roms/cps1/.shared/<crc32>.bin` — lowercase hex, no original name in it.
   A chip may be written to `.shared/` unconditionally rather than only when a
   second set appears; the device deduplicates by hash either way.
5. **Validate completeness at upload time and say so then.** If the upload is a
   clone archive alone, the correct message is *"wofj: 4 of 10 chips missing —
   needs the parent set (wof / wofr1)"*, before the file is ever copied to a
   card. If the parent's chips are already in `.shared/`, complete it silently.
6. Discard the PAL dumps (279 B files) — the device ignores them.

### retro-go-sd (firmware)

1. `ACTIVE_FILE->path` is the game folder.
2. Scan it; consider only files of exactly 512 KB. Fetch missing chips from
   `.shared/<crc>.bin` by hash, never by scanning that folder (§2.2).
3. Cache each with `odroid_overlay_cache_file_in_flash()` → XIP pointer, no RAM.
4. CRC32 the **flash copy**, not the SD file — hashing 5 MB off the card would
   cost seconds on every launch, and a cached launch reads nothing from SD.
5. Match the pool against the romset table; assign program and GFX slots.
6. On failure, name the closest set and the number of missing chips **on
   screen** — `draw_error_screen()`, not just the log.

## 5. Cost, so nobody re-litigates it

| | |
|---|---|
| RAM for ROM | **0** — everything is XIP out of external flash |
| External flash per game | ~5 MB (1 MB program + 4 MB graphics) |
| Files cached at once | 11 (2 prg + 8 gfx + the core's XIP blob) |
| SD reads on a warm launch | 0 — the flash cache hits |

`MAX_LIVE_FILES` in `gw_flash_alloc.c` was raised 8 → 16 for this: eleven
addresses are held at once and the ninth would have gone unprotected against
the cache ring, which is the Super Metroid bug's shape with the hole landing in
a bitplane.
