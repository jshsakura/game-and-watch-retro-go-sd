# Video player (AVI / MJPEG + MP3)

`main_video.c` → `video_play.c` (transport + pacing) → `avi.c` (demux), `video_decode.c`
(HW JPEG), `video_audio.c` (MP3 → resample → SAI ring).

Two failures shipped here, and both came from the same blind spot: **the video player is the
only thing in the tree that drives these two subsystems at their limits**, so a change that
looks safe everywhere else lands here first.

## The HW JPEG decoder's input-size contract (this killed playback outright)

`hw_jpeg_decoder.c` is shared with the launcher's cover art and with the `.gw` artwork loader.
Passing it the **exact** length of a JPEG is a different code path from passing it a padded
bound, and only the video player passes the exact length:

| caller | `SrcSize` it passes | vs. real JPEG |
|--------|--------------------|---------------|
| covers (`gui.c`) | `COVER_SIZE`, the cache slot size | **larger** |
| `.gw` artwork (`gw_romloader.c`) | rest of the ROM file | **larger** |
| **video (`video_decode.c`)** | **the AVI chunk's size** | **exact** |

HAL calls `HAL_JPEG_GetDataCallback()` the moment it has pushed the last byte of the source
(`JpegInCount == InDataLength`, `stm32h7xx_hal_jpeg.c:3584`). For an image handed over whole,
**that is the normal end of the stream, not an error** — the peripheral still has the tail in
its input FIFO and completes from there. It fires only when HAL actually exhausts the buffer,
so a padded caller never sees it and an exact-size caller sees it on *every good frame*.

Treating it as a rejection (`decode_rejected = 1`) therefore failed 100% of video frames while
covers and `.gw` artwork kept working — which is exactly what makes the bug hard to read: three
callers, one dead, and the two live ones "prove" the decoder is fine. That shipped in
`testbed-full-20260710-1025` and the symptom on screen was `decode st=5 rc=1`.

**Rules:**
- The callback's only job is `HAL_JPEG_ConfigInputBuffer(hJPEG, NULL, 0)` — report end-of-input.
  Omitting that is the *other* bug (HAL rewinds `JpegInCount` to 0 and replays the buffer for
  ever). Do not add anything else to it.
- A genuinely truncated image never reaches EOC, so it fails through `JPEG_DECODE_TIMEOUT_MS`.
  That, not the callback, is where a bad frame is supposed to die.
- Change anything in `hw_jpeg_decoder.c` and you must test **all three** callers. A cover
  rendering correctly says nothing about video.

## Playback follows the audio DMA clock

At normal speed, after the first decoded MP3 samples, the presentation timeline
uses `music_audio_clock()`: the actual SAI DMA sample count plus its IRQ tick.
SysTick interpolates only between callbacks, bounded to one DMA half-buffer;
each callback corrects the estimate. A coherent pair prevents an IRQ between
the two reads from adding a spurious audio block. Volume zero still advances
this clock, as does an underrun. Pause and emulator ownership do not.

Silent clips and 0.5x/2x playback use SysTick. A missing DMA callback for three
half-buffers also falls back to SysTick. Transitions preserve monotonic time;
start, seek, pause and speed changes re-anchor the frame schedule. Speed changes
flush the MP3 reservoir and resampler phase so stale 1x audio is not replayed.
The audio-clock API lives in the resident firmware: install matching firmware
and SD cores together when updating this player.

The 4096-sample audio ring still uses the existing PI resampler trim within ±1%
and its oldest-sample valve at 2560 samples. These bound buffering when source
chunks or hardware timing differ. The valve is recovery, not proof that audio
was preserved: `g_video_audio_drops` counts discarded samples. Reset the PI state,
MP3 reservoir and resampler phase whenever the ring is flushed.

Prefetch headroom gates only audio payload work, rather than all AVI headers
and video reads. A video chunk can therefore be prefetched with a full audio
ring. An intervening audio chunk is retained as `pf_audio_left` until there is
headroom; wait-time decoding consumes one 512-byte piece per step, while a
forced fetch completes it. Audio remains in demux order.

## The clock, and the frame-size cliff

Two things this app had that nothing pointed at until 0728.

**It never asked for the clock.** GBA, SNES, Virtual Boy and WonderSwan all call
`common_emu_auto_oc()`; the player that does a blocking SD read, an MJPEG decode, an
MP3 decode, a resample and a full-screen blit inside every 1/fps ran at the stock
280 MHz. It takes level 2 (340 MHz) now. That is worth more here than +21% suggests,
because the SD read is a CPU-driven SPI loop -- the clock speeds up the bytes, not
just the arithmetic. **Level 2 and not the core-private level 3**: a clip is
sustained load for ten minutes, and 353 MHz is exactly what proved unstable under
sustained load elsewhere.

**A frame bigger than a slot is silently undrawable.** `VIDEO_FRAME_MAX` is 64 KB
and the scratch is divided into exactly three of them; a larger frame is enqueued as
a failure marker (`slot = -1`) and never drawn. That is correct -- there is nowhere
to put it -- and on screen it is indistinguishable from SD or decode judder. The HUD
now reads `sz=<last>/<max>k big=<count>`: the largest frame the clip contains and how
many did not fit, both reset per clip. **Read `max=` before arguing about the slot
size.** The companion encoder uses VBV rate control to reduce peaks and explicit
4:2:0 sampling to fit the JPEG workspace. VBV is not a hard per-frame guarantee;
check `big=` for every clip regardless of where it was encoded.

## Recovering from an expensive frame

Dropping only the JPEG decode does not recover when SD reads dominate: the old
loop first read every payload in full, then checked the presentation deadline.
An overloaded clip could present its first two frames and spend the rest of
playback reading frames it would immediately discard.

`pf_fetch()` now passes the current presentation deadline to the forced
`pf_step()` path. Completed queued frames are still delivered without a read.
For an unbuffered frame whose deadline has passed, the payload (or the unread
remainder of a partial prefetch) is discarded using `PF_DROPPED`; `avi_next()`
seeks past it on the next call. Audio chunks are still fed in order. Start,
seek, pause and speed changes re-anchor timing; an unanchored fetch has no
discard deadline. Keep the signed tick comparison so timer wrap still works.

`tests/test_video_play.c` checks unread payload bytes, slot release, audio
ordering and timer wrap. `tests/test_video_timing.py` runs the existing rig
natively with the real playback code: sustained read overload must recover,
and six minutes of normal playback / 2% clock mismatch must keep presenting
every frame. These are injected timing models, not STM32 speed measurements.

An EMA of successful JPEG decode time skips frames unlikely to finish within
one frame, enabled when decode cost exceeds half the frame budget. This avoids
penalising cheap JPEGs on clips whose bottleneck is SD reading.

The HUD now compares `rd`, `pf`, `jpg` with the frame budget, and distinguishes
`late` deadline drops from `fail` rejected/oversized frames. `max` decode time
resets per clip. A rising `late` count with high `rd` or `jpg` indicates missed
deadlines; `big` identifies frames exceeding the slot capacity; `ring` exposes
audio buffering. `aud`/`dmx` separate MP3 work from demux work, and `clk` shows
whether presentation currently follows SAI or SysTick. None alone establishes the cause of a specific device report.

## Resume positions

`video_resume.c` -- one line per clip in `/data/video_resume.txt`, read after
`avi_open()` and written **only once playback has stopped**. The player must not
touch the SD while it is decoding; by the time `video_resume_put()` runs the audio is
stopped and the codec is down, and the demuxer is still open because it is what still
knows the position.

Three edges, each a test case in `tests/test_video_resume.c` rather than an opinion:

- a position in the first ~10 s is ignored (resuming four seconds in is worse than
  starting over)
- a position within ~5 s of the end **erases** the entry -- otherwise "continue" drops
  you in the credits, which looks exactly like the clip refusing to play
- a rewrite must not lose the other clips

It **streams through a temp file and commits with `f_rename`**. Collecting the
surviving lines in a `static char[32][266]` first is 8.5 KB of BSS this overlay does
not have -- the linker says `Error: MUSIC BSS overflow` and refuses. Same shape
`rg_favorites.c` uses, for the same reason, and crash-safe as a bonus.

## Verifying on device

Enable the debug HUD and play a long clip. Compare `rd`, `pf`, `jpg`, `aud` and
`dmx` with the frame budget. `clk=SAI` confirms audio timing is active; `clk=tick`
is expected for silent clips and other speeds. `late` counts deadline recovery,
`fail` decode/read failures, and `big` frames larger than the slot. The ring
should remain bounded; reaching zero can indicate an underrun.

`tests/test_video_timing.py` uses the real playback sources with injected SD,
JPEG and DMA timing: normal playback, ±2% audio-clock error, sustained read
overload and decode overload. It verifies presentation counts and elapsed
media time. `tests/test_audio_dma_clock.c` exercises the resident audio driver's
actual callback path. These checks do not measure STM32 peripheral speed.

The existing Cortex-M7 rig can also run these sources under QEMU. Hardware
verification must use an actual clip and record the HUD / device behaviour.
