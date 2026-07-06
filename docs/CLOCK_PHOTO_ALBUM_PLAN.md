# Clock — Photo Album & the "tail-borrow" RAM trick

Locked design from the 07-06 session. The clock stays a **launcher-context**
app (instant open, no reboot, like the stock G&W) and gets rich photo backgrounds
by **borrowing the unused tail of the launcher's ROM-list buffer** — no overlay,
no reboot, no re-scan.

## Decisions (locked)

- **No homebrew/overlay.** Overlay would give ~724K but costs a reboot to open
  and a big risky migration. The stock G&W switches clock↔game instantly because
  it is one program; our launcher-context clock already switches instantly. Keep
  that.
- **Buttons**: TIME → clock view, GAME → home (launcher), PAUSE → settings.
- **Modes kept as-is**: pomodoro / timer / stopwatch / alarm / themes / faces /
  built-in pixel scene.
- **New**: photo-album background, RTC time-set in settings, photo interval
  setting, legibility scrim, TIME→direct / GAME→exit wiring.

## The RAM problem and where it went

RAM_EMU pool = 724K (FLASH build). Free while the clock runs ≈ 232K. The hog:

- `shared_files = ram_calloc(SHARED_ROM_SLOTS=1000, sizeof(retro_emulator_file_t))`
  (`rg_emulators.c:756`). `sizeof ≈ 540 B` (dominated by `char name[256]` +
  `char path[256]`). **1000 × 540 ≈ 527K reserved up-front**, regardless of how
  many ROMs exist. Every emulator tab shares this ONE buffer (`p->roms.files =
  shared_files`); only the current tab's `count` slots are live at a time.

## The trick — borrow the WHOLE buffer, re-scan on exit (chosen)

Borrow the **entire** `shared_files` region (527K) as the clock's photo arena,
regardless of tab size. Deterministic and simple: always ~527K, no count-math,
no fallback branching.

```
enter clock:  use &shared_files[0] .. +527K  as photo memory (borrow in place)
exit  clock:  gui_event(TAB_REFRESH_LIST, current_tab)  → rebuild list + listbox
```

- Allocator untouched (no `ram_malloc`/`ram_mark`); `global_items`/covers above
  are untouched. Only `shared_files` contents are overwritten.
- The listbox text points into `shared_files` (`items[i].text = roms.files[i].name`,
  `rg_emulators.c:645`), so it dangles WHILE the clock runs — fine, the launcher
  isn't drawing the list then — and is rebuilt by the exit re-scan.
- ~527K arena ⇒ 3 full 320×240 photos + dissolve scratch, on ANY tab.

**Rejected: tail-only borrow** (use just `shared_files[count..1000)` to skip the
re-scan). Clever but "애매" — usable size depends on the current tab's ROM count,
needing a fallback path for monster tabs. Full reclaim + re-scan is simpler and
the re-scan is cheap (see below).

## Fallback — and re-scan is "natural" by construction

If the tail is too small (monster tab, or the clock wasn't reached from a ROM
tab), reclaim the **whole** `shared_files` (527K) and re-scan the current tab on
exit. That re-scan is `gui_event(TAB_REFRESH_LIST, tab)` — **the exact code path
the launcher already runs every time you switch tabs**, so it "flows naturally"
with zero special masking; it reads like landing on a tab. So:

- normal tab → tail is enough → **no re-scan, instant**;
- monster tab / max-richness → full reclaim → **re-scan == one tab-switch**, still
  far faster than a reboot.

Always keep a solid-background fallback; never assert on the launcher's memory.

## Photo album

- Fixed folder `/clock/album/` of JPEGs (phone photos welcome — see below).
- Decode ONE at a time into a 320×240 RGB565 buffer carved from the tail.
- **Phone photos**: resolution is the only limit (a 12 MP JPEG decodes to ~24 MB).
  Handle either by pre-sizing on the server (game-and-what `fill` = scale-to-cover
  + center-crop) OR on-device via `tjpgd` streaming decode with `JD_USE_SCALE`
  (already in-tree) shrinking straight into the 320×240 buffer. Format is not the
  blocker; JPEG (HW + tjpgd) works, PNG needs lupng made resident.

## Transition — mosaic block dissolve (no hard cut, no forced black)

Requirement: no abrupt "뿅" cut. Chosen: **direct mosaic block dissolve** — the
old photo's blocks are replaced by the new photo's blocks one region at a time,
straight to the new image (going through black is allowed but not required).

- Needs both images available during the dissolve. Hold the incoming photo in a
  second tail buffer (tail has room on a normal tab) or in the spare LCD
  framebuffer (RAM_UC, freed by single-buffering during the ~0.5 s transition,
  writes synced to vblank to avoid tearing).
- Block copies are cheap `memcpy`s; interval between photos is user-set.

## Legibility scrim

Photos are arbitrary/bright, so digits can wash out. `scrim_for_digits()` in
`rg_clock.c` darkens a feathered vertical band (date→digits→status) toward black
before the face is drawn. Only over a photo background. (Already stubbed in.)

## Settings additions

- Background: off / theme / pixel scene / **photo album**.
- **Photo interval** (seconds) — user-selectable.
- **Set time** (RTC): reuse `rg_rtc.c` `HAL_RTC_SetDate/SetTime`; an editor like
  the alarm editor.
- Alarm: existing editor.

## Verification split

- Host-testable: scrim look (clock_preview), tail-size math + fallback (unit
  test), folder-scan logic (stubs).
- Device-only (flash + test): the tail borrow, HW/tjpgd JPEG decode, the mosaic
  dissolve, RTC write. Build defensively; never assert on the launcher's memory.
