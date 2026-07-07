# Upstream-relevant bug fixes — index

Small, single-purpose fixes split out from the SD-card fork for review against
`sylverb/game-and-watch-retro-go-sd` `main`. Each lives on its own branch off
`upstream/main`, is one commit, and carries no fork-only feature code
(no favorites/clock/music/video, no per-core ports, no MAX_EMULATORS bumps).

Each was checked against upstream's own code to confirm the bug exists there and
that sylverb hasn't already fixed it. Verification was done with a stock Debian
`arm-none-eabi-gcc 13.2.1`; the upstream base has some unfetchable submodule
pins, so a full firmware link isn't possible here — verification is at the
touched-translation-unit object level plus `make` parse checks.

| Branch | Fix | Severity | Verification | Already fixed upstream? |
|---|---|---|---|---|
| `fix/storage-sprintf-bound-upstream` | `rg_storage_get_adjacent_files()` builds `best_prev/best_next` with unbounded `sprintf` into 255-byte buffers; `dir` + `fname` can reach ~510 bytes and overflow the stack. Switch to `snprintf` (the FrogFS branch already does). | High (stack overflow, reachable from launcher) | `rg_storage.o` compiles, `SD_CARD=1`. `-Wformat-truncation` confirms the exact overflow. | No — upstream `rg_storage.c` still uses `sprintf` at both sites (lines 399/407). |
| `fix/storage-littlefs-scandir-upstream` | On flash builds `rg_storage_scandir()` only walks FrogFS, so writable LittleFS dirs (`/retro-go/saves`, `/music`, …) always enumerate empty (e.g. cheat-state cleanup never finds its files). Adds a public `gw_fs_is_frogfs_path()` probe, caller-owned `fs_diropen/fs_dirread/fs_dirclose`, and a LittleFS walk branch. | Medium (writable-dir enumeration silently no-ops on non-SD builds) | `rg_storage.o`, `gw_littlefs.o`, `syscalls.o` all compile, `SD_CARD=0`. | No — upstream scandir `#else` branch is FrogFS-only; `is_frogfs_path` exists but scandir doesn't consult it. |
| `fix/littlefs-image-lang-upstream` | LittleFS image recipe never passes `--include`, so it defaults to `("cores",)` and omits `/lang`. On flash builds every non-English UI silently falls back to English. Pass `--include cores --include lang`. | Medium (i18n broken on flash builds) | `make` parses; matches `gen_littlefs_image.py --include` semantics (DEFAULT_DIRS is replaced, hence cores is re-listed). | No — upstream `LITTLEFS_IMAGE` recipe has no `--include`. |
| `fix/gittag-littlefs-dep-upstream` | `gw_littlefs.c` includes the generated `gittag.h`, but only 4 other objects declare that prerequisite. Under `make -j` the build intermittently dies with `fatal error: gittag.h: No such file`. Add the one-line dep. | Medium (intermittent `-j` build break) | `make` parses; race reproduced live here (a `-j`-order object build hit exactly this missing `gittag.h`). | No — upstream lists the dep for error_screens/main/rg_main/rg_emulators only. |
| `fix/wget-retry-upstream` | HAL/CMSIS/SDK headers are fetched with plain `wget -q`; parallel builds trip GitHub raw's 429 rate limit, which aborts the whole download step. Route through `WGET_SDK` with `--tries/--waitretry/--retry-on-http-error`. | Low/Medium (fragile first build / CI) | `make` parses. | No — upstream uses plain `wget -q` in all three rules. (Fork's CI-workflow parts are fork-specific and were left out; only the shared Makefile.common rule is here.) |
| `fix/savestate-label-no-strftime-upstream` | Savestate slot labels format the mtime with `strftime`, the firmware's only caller, which drags ~3.6 KB of newlib locale tables into the tight 256 KB intflash bank. Build the fixed `dd/mm/yyyy hh:mm:ss` string with `snprintf` (byte-identical output). | Low (flash footprint) | `odroid_overlay.o` compiles, `SD_CARD=1`. | No — upstream `odroid_overlay.c:1540` uses `strftime`. |
| `fix/newlib-toolchain-quirks-upstream` (optional) | Two portability defines so the firmware builds with a stock distro `arm-none-eabi`/newlib (not only the pinned Docker toolchain): `-DPATH_MAX=256` for frogfs.c and `-D__int64_t_defined=1` for PokeMini's `retro_common_api.h`. Both are no-ops where the toolchain already behaves. | Low (developer experience) | `make` parses; `-DPATH_MAX=256` was empirically required to compile the touched TUs on this Debian toolchain. | N/A — upstream's canonical Docker build doesn't hit these. Kept in this fork on purpose: it lets anyone (including the fork owner) build with a stock distro `arm-none-eabi` instead of the Docker image. Offered upstream only as a take-it-or-leave-it convenience. |

## Rejected candidates

| Candidate | Why rejected |
|---|---|
| `gui_carousel_min` / COVERFLOW-gated theme getter | `gui_carousel_min` does not exist in upstream — it is fork-added, so there is no upstream bug to fix. |
| `GW_RTC_RestoreIfLost` skipped on `SD_CARD=0` | `GW_RTC_RestoreIfLost` (and the RTC snapshot/restore mechanism) does not exist upstream — fork-only, dropped. |
