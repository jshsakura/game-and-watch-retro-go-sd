# Music Player — plan / resume notes

Branch `feat/music-player` (fork PR #2). Goal: a **complete, native-feeling MP3
player** ("마치 원래부터 그런 용도로 나온 제품처럼"). All code in
`Core/Src/porting/media/main_media.c`.

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

## Other branches
- `feat/media-browser` (PR #1): full media browser (txt/image/video) experiment.
- `feat/media-converter`: web converter (image resize, txt→UTF-8 incl EUC-KR,
  video→MJPEG). Could add MP3 cover shrink+re-embed.
- `ko-kr-update`: Korean i18n refinements.

## Build/test
Intra-fork PR build-test uploads `media-browser-build` (firmware + `gw_update.tar`
with `Music.bin`). Download from the PR's Actions run → Artifacts.
