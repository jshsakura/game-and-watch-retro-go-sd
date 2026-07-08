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
(`gw_fs_is_frogfs_path()`), so LittleFS directories (music, video) enumerate
correctly. (The clock's own media backgrounds are compiled out on flash builds
— see the clock matrix below.)

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
- Clock user-media (photo album / GIF background / MP3-WAV alarm) is compiled
  out on flash builds rather than shipped broken: a card-less unit cannot
  receive the files, so the background picker offers only Off/Scene and the
  alarm is beep-only (see the clock matrix below).

## Clock app: SD vs flash-only matrix

The clock is a first-class feature on card-less units (it may well BE the
device's day job there), so every clock feature branches deliberately:

The user-media parts of the clock (photo album, GIF background, MP3/WAV alarm)
all need a writable place to **receive** files, which a card-less unit has no
way to do. Rather than pretend otherwise, they are compiled out entirely on
`SD_CARD=0` (their `.c` files drop from the build, and the call sites in
`rg_clock.c` / `rg_main.c` are `#if SD_CARD == 1`). The background picker then
offers only Off/Scene and the alarm is beep-only.

| clock feature | SD build (card) | flash build (LittleFS) |
|---|---|---|
| faces, themes, pixel scenes, timers, pomodoro | RAM/flash only — identical | identical |
| settings / alarms / auto-dim (`/clock/clock.cfg`) | SD, editable on a PC | LittleFS, device-side edits persist |
| background picker | Off / Scene / GIF / Photo | **Off / Scene only** (GIF + Photo compiled out) |
| photo album (`/clock/album/*.565` or `*.bmp`) | drop files on the card (web tool, or any 320x240 24/32-bit BMP) | **not available** (compiled out — no way to receive files) |
| GIF background (`/clock/gif/*.gif`, picked in settings) | drop on card / web tool push | **not available** (compiled out) |
| alarm sound (`/clock/alarm/*.mp3\|*.wav`, picked in settings; GAME previews the file ~10s) | Beep or any file on the card (`/cores/clockmp3.bin` decoder) | **synth beep only** (MP3/WAV alarm compiled out; `clockmp3.bin` not baked) |
| RTC restore after battery drain (`/save/clock.bin`) | snapshot on card | snapshot on LittleFS (restore runs on both builds) |
| hibernate save | dedicated `off.sav` | dedicated `off.sav` (`OFF_SAVESTATE=1` default) |

## Known limitations

- **User-media features are SD-card only.** The clock's photo album, GIF
  background and MP3/WAV alarm are compiled out on flash builds; the Music and
  Video apps stay visible but show an empty list. A card-less unit has no way to
  receive files (no `sdpush`/`sdcopy` equivalent for LittleFS), so there is no
  supported path to add media to a flash build — this is by design, not a gap to
  bake around. The LittleFS image ships only what a card-less unit actually
  needs: `cores` and `lang` (plus the writable `save`/`settings`/`clock.cfg`).
- `release` remains SD_CARD=1-only: an SD_CARD=0 image embeds ROMs and is not
  redistributable.

## Test coverage

`tests/run.sh` compiles `rg_storage.c` for **both** backends (FatFs stubs and
FrogFS/LittleFS stubs) plus favorites/clock suites; CI job `host-tests` runs it
on every push.
