# Non-SD (flash-only) builds — SD_CARD=0

State of the fork's `SD_CARD=0` support after the 2026-07-07 full sweep. This is
the configuration a Game & Watch **without** the SD-card mod runs: the firmware
lives in internal flash, and the external flash holds a read-only **FrogFS**
image (roms/bios/fonts/covers) plus a small RW **LittleFS** partition
(cores, lang, saves, settings, clock data).

## Building

```sh
make -j$(nproc) SD_CARD=0 INTFLASH_BANK=2 EXTFLASH_SIZE_MB=<N> all
```

- `all` now produces `build/frogfs.bin` **and** `build/littlefs.bin` alongside
  the intflash images — flash all of them (`make flash` does).
- `EXTFLASH_OFFSET + EXTFLASH_SIZE` must be a power of two; size the flash to
  your chip (the 1 MB default only fits a token ROM set).
- ROM payloads are LZMA-packed by default (`COMPRESS=lzma`); `COMPRESS=0`
  trades external-flash space for ~6 KB of intflash decoder.
- The intflash bank is 256 KB and tight. `COVERFLOW=0` builds link since the
  strftime diet (see below); prefer it on SD_CARD=0.
- CI: `.github/workflows/package.yml` job `build-sd0-test` keeps this
  configuration green on every push/PR.

## Path routing (syscalls.c, SD_CARD=0 branch)

| first path segment | backend | writable |
|---|---|---|
| `roms`, `covers`, `bios`, `fonts`, `font`, `cores/pico8.ro` | FrogFS | no (`EROFS`) |
| everything else (`save`, `data`, `clock`, `favorites.txt`, `lang`, `music`, `video`, `CONFIG`, `cores/*.bin`) | LittleFS | yes |

`rg_storage_scandir()` picks the backend per directory with the same rule
(`gw_fs_is_frogfs_path()`), so LittleFS directories (music, video, clock
album) enumerate correctly.

## What the 2026-07-07 sweep fixed

Link-level (SD_CARD=0 did not build at all since the fork's feature work):

- FrogFS `PATH_MAX` and PokeMini `PRI*64` fallout of the Debian arm-none-eabi
  toolchain quirks (`-DPATH_MAX=256`, `-D__int64_t_defined=1` — both no-ops on
  toolchains without the quirk).
- SD-only symbols referenced from shared code: diag hooks (`sd_save_log`,
  `doom_trace_raw`, …) are no-ops on flash builds; `gw_fs_relpath_prefix`
  exists on both; MSX sleep-wake SD reopen, video HUD `sdcard_hw_type`, and
  `gui_carousel_min`'s COVERFLOW-gated settings call are gated.
- Clock/favorites moved off raw FatFs calls: `f_mkdir` → `rg_storage_mkdir`,
  favorites delete/rename per backend.
- pce-go's SCSI hooks link against `pce_cd_stubs.c` (no CD unit) since the CD
  stack is SD-only.
- `STM32H7B0VBTx_FLASH.ld` got the C64 overlay `.init_array` block that only
  existed in the SDCARD script.
- 256 KB intflash overflow (384 B): replaced the single `strftime()` call with
  manual formatting (−3.6 KB of newlib locale tables).

Runtime-level (found by tracing every feature with no card present):

- **LittleFS dirs were unlistable** — `rg_storage_scandir` only walked FrogFS,
  so music/video/clock-album were permanently empty. Fixed with the
  per-backend walk (`fs_diropen/fs_dirread/fs_dirclose`).
- **Favorites remove always failed** — the temp-file rewrite needs two open
  files, LittleFS allows one (`MAX_OPEN_FILES 1`). Flash builds now filter the
  list in RAM and rewrite in place.
- **`/lang` was never packed** — language `.bin`s are generated but weren't in
  the LittleFS image, so every non-English UI silently fell back to English.
  `gen_littlefs_image.py` now gets `--include cores --include lang`.
- **PC Engine CD hidden** — the tab is SD-only (`#if SD_CARD == 1`), and
  `pce_osd_getromdata()` refuses `.cue` files on flash builds instead of
  booting the cue text as a HuCard.
- **Sleep no longer clobbers save slot 0** — `OFF_SAVESTATE=1` is the
  SD_CARD=0 default, so hibernate writes `/save/off.sav` like SD builds.
- **`make all` builds `littlefs.bin`** — flashing a firmware without its
  matching cores image fails every emulator launch.
- Clock "Photo" background reports `(no photos)` in the settings menu when the
  album is empty, mirroring the GIF diagnostics.

## Known limitations

- No post-flash content push for LittleFS (no `sdpush` equivalent): adding
  album photos / `bg.gif` / `alarm.mp3` / music / videos means rebuilding
  `littlefs.bin` and reflashing that partition. Baking is one variable:
  `make SD_CARD=0 LITTLEFS_INCLUDE="cores lang clock music video" all`
  packs those `sd_content/` dirs into the image (cores+lang are the
  required minimum). Saves live in the same partition — reflashing it
  loses them.
- Music/Video entries stay visible and show an empty list until content is
  baked in (kept: they work fine once `/music`, `/video` are packed).
- `release` remains SD_CARD=1-only: an SD_CARD=0 image embeds ROMs and is not
  redistributable.

## Test coverage

`tests/run.sh` compiles `rg_storage.c` for **both** backends (FatFs stubs and
FrogFS/LittleFS stubs) plus favorites/clock suites; CI job `host-tests` runs it
on every push.
