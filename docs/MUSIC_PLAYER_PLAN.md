# Music Player — plan / resume notes

Branch `feat/music-player` (fork PR #2). Goal: a **complete, native-feeling MP3
player** ("마치 원래부터 그런 용도로 나온 제품처럼"), beautiful, with commercial-MP3
features, max performance on weak HW, and on-screen button hints always shown.

## Session 2 — full rewrite + modular split (host-tested, pending CI)
Split the 1375-line monolith into cohesive modules (all <800 lines):
- `media_id3.{h,c}` — ID3v2.2/2.3/2.4 reader: title/artist/album/album-artist/
  composer/genre/year/track/comment + **USLT lyrics** + APIC cover, all decoded
  to **UTF-8** (latin1/UTF-16±BOM/UTF-16BE/UTF-8). `id3_read_lrc` for sidecar.
- `media_audio.{h,c}` — minimp3 decode + ring + pump + **seek** + duration.
- `media_cover.{h,c}` — dimmed full-screen **backdrop** + crisp **card** + thumb.
- `media_lyrics.{h,c}` — pure LRC/USLT parser (synced highlight). **host-tested.**
- `media_ui.{h,c}` — primitives + now-playing (backdrop+card+faux-bold title +
  artist·album) + info screen + lyrics view + **always-on hint bar**.
- `main_media.c` — browser, favourites (SD-persisted), playlist, player loop.

Done from the TODO below: 1 (controls), 2 (seek), 8 (beautiful design),
9 (lyrics), 10 (full metadata + info screen). Plus **favourites** (★ shortcut,
heart, persisted to `<root>/.favourites`), **repeat off/all/one**, **player menu**
(PAUSE → favourite/repeat/shuffle/brightness/info/lyrics via odroid_overlay_dialog),
**volume** (▲▼), **screen-off** (POWER), **always-on hint bar**.

Perf: hint bar moved to the per-track STATIC layer (no per-frame text-width
measuring); dynamic layer repaints only the top bar + transport strip; list-row
metadata cached (16 slots). Glyphs ▲▼◀▶ (Geometric) / ♥♪ (Misc Symbols) come
from the i18n font; play/pause/knob/volume-pips drawn geometrically.

Tests (`tests/run.sh`, host gcc, red-green): media_id3 (encoding/synchsafe/APIC)
+ media_lyrics (LRC/synced) — **38 checks green**. All modules pass host
`-fsyntax-only` (caught + fixed a `void*`-indexing bug in the knob drawer).

Controls: A play/pause · ▲▼ volume · ◀▶ tap=track / hold=seek-scrub · TIME
shuffle · GAME cycle view (now-playing→info→lyrics) · PAUSE menu · POWER
screen-off · B list. Still TODO: 4 (launcher icon), 6 (verify on device),
non-blocking menu (audio currently gates during the overlay dialog).

## Original plan
All code in `Core/Src/porting/media/main_media.c`.

## Current state — CI green (run 27252121106, commit 951045aa)
- Homebrew app **"Music"** (renamed from Media). Scans **`/music`** (falls back to `/media`).
- **Rich library list**: each row = album-art thumbnail + title + duration.
  - Thumbnails via **TJpgDec** scaled decode → any-size JPEG cover fits in ~8KB.
  - Duration estimated from first-frame bitrate + file size.
  - Themed with `curr_colors`. Lazy metadata cache (`g_meta`, 16 slots).
- **Now-playing**: cover (TJpgDec scaled, ID3 embedded or sidecar; PNG via lupng),
  themed panel, progress bar, elapsed/remaining.
- **Playback**: ring-buffered MP3 (no stutter), shuffle/sequential, prev/next,
  pause, auto-advance.
- Current (interim) controls: A=play/pause, ◀▶=prev/next, ▲=shuffle, B=back.
- Vendored libs (all permissive, notices kept): minimp3 (CC0), TJpgDec (ChaN),
  lupng (MIT), miniz (MIT) — see `Core/Src/porting/media/THIRD_PARTY.md`.

## Verified APIs (use these next session)
- Buttons (G&W → odroid, from `odroid_input.c`): **PAUSE → `ODROID_INPUT_VOLUME`**,
  GAME → `START`, TIME → `SELECT`, POWER → `POWER`, plus UP/DOWN/LEFT/RIGHT/A/B.
- Volume: `odroid_audio_volume_get()` / `odroid_audio_volume_set(int)`; range 0..9
  (`ODROID_AUDIO_VOLUME_MAX = 9`).
- Backlight: `lcd_backlight_off()` / `lcd_backlight_on()` / `lcd_backlight_get()` /
  `lcd_backlight_set(uint8_t)` (`gw_lcd.h`).
- Firmware menu: `odroid_overlay_settings_menu(extra_options, repaint_cb, flags)`
  (`odroid_overlay.h`) — needs a parameterless repaint callback.
- Theme: `curr_colors->{bg_c,main_c,sel_c,dis_c}` (`gui.h`).
- `HAL_GetTick()` for ms timing (or count loop iterations ≈ 20ms each).

## TODO — final control scheme + features
1. **Remap controls**
   - A = play/pause
   - ▲ UP = volume up, ▼ DOWN = volume down (`odroid_audio_volume_set`)
   - ◀ LEFT: **tap** = prev track, **hold** = rewind (seek −)
   - ▶ RIGHT: **tap** = next track, **hold** = fast-forward (seek +)
   - SELECT (TIME) = shuffle toggle  ← move off UP
   - VOLUME input (PAUSE) = open `odroid_overlay_settings_menu` (firmware menu)
   - POWER = toggle **screen off** (`lcd_backlight_off/on`), music keeps playing;
     any key → `lcd_backlight_on` + resume UI
   - B = back to list   ·   START/GAME = free (candidate: repeat mode)
2. **Seek**: store `g_track_off` + audio size in `track_open`; `seek_to(sec)` =
   `fseek(off + frac*size)`, `mp3dec_init` (resync), clear ring, `mp3_pump`,
   `played = sec*48000`. Tap-vs-hold via frame counters (tap < ~300ms; else seek
   mode ~±0.25s/frame).
3. **Menu repaint**: split `draw_now_playing` into panel-draw (no swap) + swap;
   make player state global (path/name/sec/total/paused/shuffle) so the repaint
   callback can redraw cover + panel.
4. **Music app launcher ICON** (user request): homebrew entries currently share
   `RG_LOGO_HEADER_HOMEBREW`. A per-app icon needs an icon asset + launcher
   wiring — investigate the RG_LOGO graphics system / whether per-file homebrew
   icons are feasible.
5. **Cleanup**: remove dead viewer code (txt/image/avi: `view_text`,`render_page`,
   `load_text_file`,`char_px_width`,`utf8_len`,`view_image`,`show_jpeg`,`show_png`,
   `view_avi`,`avi_*`,`wait_for_b`,`draw_center_msg` if unused). Currently DCE'd
   but warns; remove for a clean file. Consider splitting `main_media.c`.
6. **Verify on device**: confirm ID3 album art shows (user reported "안나온다" —
   likely an older pre-TJpgDec build; the rich list now exercises the same path).
   If still missing: TJpgDec is **baseline-only** (no progressive JPEG); also check
   ID3v2.2 (3-char frames) and extended-header edge cases in `extract_id3_art`.
7. **Polish**: premium/native feel — smooth track transitions, consistent theming,
   clean typography/layout.

8. **Beautiful design pass** (high priority — "디자인 아름답게"):
   - Premium now-playing: cover as a full-screen **dimmed/blurred backdrop** +
     a crisp centered cover card; elegant typographic hierarchy (title bold,
     artist/album dim).
   - Smooth **fade transitions** on track change; subtle animated progress knob.
   - Consistent theme-colour accents, rounded panels, tasteful spacing.
   - Rich list: nicer rows (album-art rounded, 2-line title/artist, right-aligned
     duration), selection highlight with accent bar.
9. **Lyrics view** ("가사 있으면 가사도"):
   - Sources: ID3v2 **USLT** (unsynced lyrics) and **SYLT** (synced w/ timestamps),
     plus sidecar **`.lrc`** files (LRC timestamp format) next to the track.
   - Parse in `extract_*` style; show a lyrics screen (toggle key, e.g. START).
   - If timestamps exist (SYLT/LRC): **auto-scroll + highlight the current line**
     against the audio clock (`played`); else plain scrollable text.
   - Lyrics text is UTF-8 (ID3 enc 1/2 = UTF-16 → convert; sidecar .lrc often
     EUC-KR → the web converter can normalise, or decode on-device).

10. **Show ALL available info — beautifully** ("보여줄 수 있는 정보는 모두"):
    - Parse all useful ID3v2 text frames: **TIT2** title, **TPE1** artist,
      **TALB** album, **TYER/TDRC** year, **TCON** genre, **TRCK** track #,
      **TPE2** album-artist, **TCOM** composer, **COMM** comment.
    - Technical info: bitrate (CBR/VBR), sample rate, channels, duration,
      file size, codec/layer.
    - Lay it out elegantly: now-playing shows title / artist / album prominently;
      an **info screen** (toggle) shows the full tag + technical table.
    - All text UTF-8 (convert ID3 enc 0/1/2/3: latin1/UTF-16/UTF-16BE/UTF-8).
    - Generalise `extract_id3_art` into an ID3 tag reader that pulls these
      frames in one pass (and the cover) into a struct.

## Other branches
- `feat/media-browser` (PR #1): full media browser (txt/image/video) experiment.
- `feat/media-converter`: web converter (image resize, txt→UTF-8 incl EUC-KR,
  video→MJPEG). Could add MP3 cover shrink+re-embed.
- `ko-kr-update`: Korean i18n refinements.

## Build/test
Intra-fork PR build-test uploads `media-browser-build` (firmware + `gw_update.tar`
with `Music.bin`). Download from the PR's Actions run → Artifacts.
