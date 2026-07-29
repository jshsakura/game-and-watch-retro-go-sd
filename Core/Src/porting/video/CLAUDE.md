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

## Nothing synchronises the two clocks (this was the "it degrades after 4 minutes")

The SAI ISR drains `video_audio.c`'s ring at the **audio PLL's real rate**. The demuxer fills it
one AVI audio chunk per *displayed* video frame — i.e. at the rate `video_play.c` paces frames
by **SysTick**. Different oscillators, different dividers: they do not agree, and the error only
accumulates in one direction.

The ring is 4096 samples ≈ **85 ms**. A 0.3% mismatch fills it in under a minute; 0.05% takes
several. And a full ring is not just an audio problem — it holds `video_audio_ring_free()` below
`PF_AUDIO_HEADROOM`, so **the prefetch gate in `pf_step()` can never open again**. Every frame
read becomes a blocking one, and playback goes from smooth to permanently stuttering. That is
the whole shape of the user report: fine at first, progressively worse, worse still on a long
clip, never recovers.

`trim_step()` closes the loop as a **PI servo**: it holds the ring near `VR_TARGET` by trimming
the resample step within ±1% (17 cents — inaudible). Consuming input slightly faster emits fewer
samples per MP3 frame and drains a filling ring. A pure-proportional trim needs a *standing* fill
error to command a standing correction, so under sustained drift it plateaus **above** `VR_TARGET`
— and a 1% mismatch demands the full ±1% deflection, only reachable with the ring already past the
gate. The **integral term** (`TRIM_KI_DIV`, `g_fill_integ`) drives that steady-state error to zero,
so the ring converges *on* `VR_TARGET`. Anti-windup pins the integrator at the authority edge, so
it never winds past what the step can express and unwinds the instant the error reverses.

Beyond ±1% mismatch the trim runs out of authority and cannot cancel the drift at all. So a
**non-latching valve** backs it up: if `ring_count()` exceeds `VR_VALVE_CAP` (2560) the oldest
sample is dropped, which **guarantees** the ring can never pin full and latch the prefetch gate
shut. Inside authority the PI keeps the ring at target and the valve is dormant (only a ~13 ms
startup blip); beyond it the valve sheds exactly the excess (~600 ms/min at a full 2% mismatch) —
a tiny periodic audio drop instead of the progressive-stutter cliff. `g_video_audio_drops` counts it.

**Rules:**
- `VR_TARGET` must stay **below** `VR_SIZE - 1 - PF_AUDIO_HEADROOM` (1695 samples), or holding
  the target would itself be what keeps the prefetcher off. These two constants are coupled;
  move one and you must re-check the other.
- `VR_VALVE_CAP` (2560) must stay low enough that a valve-pinned ring still drains below the 1695
  gate within one frame (pinned trough ≈ cap − one chunk ≈ 895). Coupled to `PF_AUDIO_HEADROOM`
  and `VR_SIZE` — re-check all three together.
- Servo on a low-passed level, never the instantaneous one: the ring swings by a whole chunk
  within one video frame, and servoing on that just modulates pitch at the frame rate.
- Reset the servo (`g_fill_ema`, `g_step`, `g_fill_integ`) wherever the ring is flushed —
  `video_audio_stop()`, which a seek goes through — or the empty ring reads as "starving" and the
  trim slams.

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
size.** The encoder keeps peaks under the ceiling with VBV rate control, so `big=`
should be 0; if it is not, the clip was made by something else.

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

`g_show_debug` HUD: `rd=` (blocking read ms), `pf=` (read hidden in the pacing wait), `jpg=`
(HW decode ms), `ring=` (servo error), and on the second line `sz=<last>/<max>k big=<count>`
(frame sizes, and how many did not fit a slot).

**`ring=` is the regression test for the drift bug.** Play a clip for 5+ minutes: it must sit
near 1200 the whole time. Climbing toward 4095 means the trim is not holding and the stutter is
coming back; falling to 0 is an underrun. `rd=` staying near 0 (with the work showing up in
`pf=`) means the prefetcher is alive — that is what the full ring used to destroy.

There is now a **QEMU Cortex-M7 rig** for the drift/latch specifically: `tools/m7_qemu_rig/rig_video.c`
+ `run_video.sh` boot the *real* video source on an emulated M7 with two independent clocks (video
SysTick vs an audio ISR at a settable ppm offset) and a synthetic AVI, printing a per-frame ledger
(ring trough, gate reopen, `rd`/`pf`). It reproduces the slowdown deterministically —
`run_video.sh <ppm> <frames>`: at ppm ≥ 10000 (=1%, the servo authority) the old code latches at a
fixed *time* independent of clip length; the PI+valve code stays bounded and never latches. That is
what proved the fix (RED→GREEN). What the rig does **not** model is absolute timing — the JPEG
peripheral, the real SAI clock and SD read latency are injected models, not hardware — so device
fps is still the device's call. Verify on hardware with a long clip; `ring=` must sit near 1200 the
whole time.
