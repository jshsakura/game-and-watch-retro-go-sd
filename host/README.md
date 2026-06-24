# Music app — host visual preview (design loop)

The Game & Watch firmware can't be screenshotted easily, so this harness renders
the **real** `media_ui.c` drawing code on a PC into PNGs. Use it to iterate on the
look *without* flashing hardware. This is the design-handoff loop.

## Run

```sh
python3 host/gen_font.py        # build the 12px preview font atlas (font_data.h)
gcc -O2 -std=gnu11 -Ihost -ICore/Inc/porting/media \
    host/preview.c Core/Src/porting/media/media_ui.c Core/Src/porting/media/media_lyrics.c \
    -o /tmp/mpv_preview
/tmp/mpv_preview                # renders /tmp/mpv/*.bin
python3 host/render.py          # -> host/preview_out/*.png  (nowplaying, list, info, lyrics)
```

Edit `Core/Src/porting/media/media_ui.c`, re-run, look at `host/preview_out/`.

## What's real vs. mocked

- **Real:** all of `media_ui.c` (layout, colours, progress, icons, ellipsis,
  scrollbar) and `media_lyrics.c`. Theme colours in `host/preview.c` (`theme`).
- **Mocked (in `host/preview.c`):** the i18n font (a Nanum/DejaVu 12px atlas —
  close to, not identical to, the device font), album art (synthetic gradient),
  and audio/cover stubs. So judge **composition, spacing, colour, hierarchy** —
  exact glyph shapes differ slightly on device.

## Hard constraints (device)

- 320×240, RGB565. Framebuffer is `uint16_t[320*240]`.
- Font is a **fixed 12px 1bpp bitmap** (`i18n_get_text_height()==12`); no size
  switching. Bigger text = integer upscale (looked blocky — reverted to crisp
  1x bold). Hierarchy comes from weight (faux-bold)/colour/spacing.
- Glyphs available: Hangul, Latin, Geometric Shapes (▲▼◀▶■●◆), Misc Symbols
  (♥♪). Arrows block (↻ etc.) is **not** available.
- Weak CPU: now-playing composes static layer once per track; the dynamic layer
  repaints only the top bar + transport strip; the list redraws only on change
  and defers metadata decode while fast-scrolling.

## Theme

`host/preview.c` sets a dark theme (`bg/main/sel/dis`). The device uses
`curr_colors` from the user's selected theme — design should work across themes,
so prefer `ui_mix()`/`ui_dim()` relative shades over hard-coded colours.

## Current state / next polish ideas

- now-playing: dimmed cover backdrop + framed card + crisp bold title +
  artist·album + slim progress + one muted hint line. Top bar: ▶ pos · shuffle/
  repeat/♥/volume.
- list: header path + `cur/total`, ★favourites, folders, thumb+title+artist+
  duration+♥, right scrollbar, muted footer hints. Long titles ellipsize.
- Open ideas: rounded card corners (needs alpha), folder/star icons instead of
  blocks, marquee scroll for the selected long title, album-grid view.
