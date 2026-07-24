# CPS-1 library layout — the contract between the library and the device

Three producers must agree on this and stay in step:

- **game-and-what** — the web library/upload side that prepares the card,
- a future **standalone Python packager** (a CLI that builds the same card
  layout without the web app), and
- **retro-go-sd** — the firmware that reads it.

This file is the agreement. No producer may change the layout without the
others. It was verified end-to-end on real hardware: the firmware opens a
Korean-named container, caches all 20 chips, resolves the romset and renders
(the on-device `/cps1_diag.txt` log proves each step).

---

## 1. Layout — ONE flat file per game

```
/roms/cps1/
    <Game Name>.cps1            ← ONE FILE PER GAME. This IS the launcher entry.
/covers/cps1/
    <Game Name>.img             ← its cover (optional), same base name.
```

- `<Game Name>` is the human display name and is shown verbatim in the launcher
  (it may be non-ASCII, e.g. Korean — the firmware opens and displays it fine;
  the file leaf is the only thing that has to match between the `.cps1` and its
  `.img`).
- **No folders, no subfolders, no `.shared/`, no MAME-named chip files, no
  archives.** A game is exactly one `.cps1` file. Anything else that was on the
  card before (loose chips, `wof.zip`) is ignored by the launcher (it lists only
  `*.cps1`).

### 1.1 The `.cps1` container format

```
<Game Name>.cps1 = chip[0] ++ chip[1] ++ ... ++ chip[N-1]
```

- Each `chip[i]` is exactly **512 KB (0x80000)** of raw MAME chip bytes, verbatim
  (no byte-swap, no padding — see §3).
- The chips are the romset's **DISTINCT** chips, de-duplicated by content CRC32
  (the same bytes appearing in two source archives are one chip). Ship the whole
  distinct pool the folder completes, not one guessed set's slice: the device
  binds by CRC and takes what its chosen set needs, so extra chips are free and
  no chip a runnable set might need is ever dropped.
- **Uncompressed, no header, no index, no order requirement.** The device
  identifies each 512 KB block by content hash (§3), so block order is
  irrelevant. The file size is always a whole multiple of 512 KB.
- Build it only when the folder completes **at least one** runnable romset;
  otherwise produce nothing (a half-a-game file that reaches the launcher and
  dies is worse than one visibly absent).

Why uncompressed and flat: the device has no inflate in the emulator path and
nowhere to put 4 MB of decompressed graphics (RAM_EMU is 724 KB). It caches each
512 KB block into external flash and reads it in place (XIP), which only works on
raw chip bytes. One file (not a folder of 20) also means one `fopen` per game —
a folder of 20 loose chips exhausted the 10-slot descriptor table on device.

## 2. Chip identification is by CRC32. Never by filename or position.

This is the part that fails **silently** if it is got wrong, so both sides must
implement it identically.

A romset's `ROM_LOAD` order is **not** its filename order:

| file | CRC32 | GFX slot |
|---|---|---|
| `tk2_gfx1.rom` | `0d9cb9bf` | 0 |
| `tk2_gfx3.rom` | `45227027` | **1** |
| `tk2_gfx2.rom` | `c5ca2460` | **2** |
| `tk2_gfx4.rom` | `e349551c` | 3 |

and in the Japanese set the upper four chips are named `tk205…tk208`, which sort
*ahead* of `tk2_gfx1…4` entirely. Assign slots in filename order and every file
loads, every size checks out, and the graphics are wrong with nothing reporting
an error. Measured: the mutation that assigns slots by filename makes
**3,584,744 / 4,194,304 bytes (85 %)** differ from the reference image.

So the device splits the `.cps1` into 512 KB blocks, CRC32s each block, and
matches those CRCs against the romset table to assign program and GFX slots.
Order in the file, and the game's display name, carry no meaning.

### 2.1 One romset table, generated — not two kept in step by hand

```
tools/cps1_romsets.json          ← THE source of truth, in BOTH repos, byte-identical
  │                                sha256 c3d444ba457abaa5a103d38282a4cd782e3751ed78601f67b50611e79fb9e75c
  ├─ retro-go-sd:   tools/gen_cps1_romset.py → Core/Src/porting/cps1/cps1_romset.c
  └─ game-and-what: backend/app/assets/cps1_romsets.json (read for build + validation)
```

`cps1_romset.c` says GENERATED FILE at the top and is not hand-edited;
`tests/run.sh` fails on either drift direction. A standalone Python packager MUST
read this same JSON — do not hard-code a second copy of the table. Record the
sha256 above when the JSON changes so a stale copy is detectable from one side.

Note `wof.zip` as distributed actually contains the **wofr1** romset (program
CRCs `11fb2ed1`/`479b3f24`, not `wof`'s). A zip's name does not describe its
contents — one more reason to key on hashes.

### 2.2 Bytes are the verbatim MAME dumps

No byte-swapping, no renaming, no padding. MAME applies `ROM_REVERSE` when it
builds its big-endian program region, and a little-endian `*(uint16*)` read of
the raw chip undoes exactly that, so the two cancel: verbatim is correct. Proof:
the first program chip begins `ff 00 ee 62 00 00 a2 71`, which reads back as
SSP=`0x00FF62EE`, PC=`0x000071A2` — a stack pointer in work RAM and an even PC.
(This exact header was confirmed on device in `/cps1_diag.txt`: `fopen OK, read
8 B: ff 00 ee 62 00 00`.)

## 3. What each side owns

### game-and-what (library) and the standalone Python packager

1. Accept MAME `.zip`(s) (a clone, optionally with its parent) or a folder.
2. Identify every 512 KB member by CRC32 against the romset table (§2.1).
3. If the inputs complete **no** runnable set, refuse and say which set is
   closest and how many chips are missing (e.g. *"wofj: 4 of 10 chips missing —
   needs the parent set wof/wofr1"*) — at upload/build time, before writing a
   card. Discard PAL dumps (279 B) and any non-512 KB member.
4. Otherwise write **one** `/roms/cps1/<Game Name>.cps1` = the distinct 512 KB
   chips concatenated (§1.1). Optionally write `/covers/cps1/<Game Name>.img`.
5. That is the entire card contribution — no chips, no zips, no `.shared/`.

The web library keeps the source `.zip`s server-side (the browser emulator wants
them); only what is *written to the card* is defined here.

### retro-go-sd (firmware)

1. The launcher lists `/roms/cps1/*.cps1`; each file is one game, shown by name.
2. On launch, `ACTIVE_FILE->path` is the `.cps1` file. Split it into 512 KB
   blocks; cache each with `odroid_overlay_cache_file_region_in_flash()` → an XIP
   pointer (no RAM), CRC32 the **flash copy**, and record it.
3. Match the block CRCs against the romset table; assign program and GFX slots.
   If several sets complete, auto-launch the first (romset-table order).
4. On any failure, draw the reason on screen and HOLD it (never `return` out of
   `app_main` — that unwinds into the torn-down launcher and BusFaults).

## 4. Cost

| | |
|---|---|
| RAM for ROM | **0** — every chip is XIP out of external flash |
| External flash per game | ~5 MB (1 MB program + 4 MB graphics) per runnable set; the container ships the whole distinct pool |
| `fopen`s per launch | 1 (the single `.cps1`) — not one per chip |
| SD reads on a warm launch | 0 — the flash cache hits |

## 5. History (do not re-litigate)

- A `.cps1` **container** was invented, then retired (`0bd923c1`) when the layout
  was a folder of raw chips + `.shared/`. It is now **back and is the contract**:
  the folder-of-chips layout cost one `fopen` per chip and exhausted the device's
  10-slot descriptor table (2 chips failed to cache with "size 0"). One flat
  uncompressed file fixed that.
- The container was briefly nested inside a per-game folder with an ASCII
  `<set>.cps1` name, to guard against a non-UTF-8 unzip mangling a Korean file
  name. The device proved a Korean-named `.cps1` opens and caches fine, so the
  folder was dropped for the simpler flat file above.
- The first on-device render crash was **not** any of the above: the blitter was
  tagged for ITCM but the runtime copy into ITCM was never wired, so the first
  blit jumped into uninitialised ITCM. Fixed by dropping the ITCM placement.
