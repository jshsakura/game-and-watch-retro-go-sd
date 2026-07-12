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

`trim_step()` closes the loop: it holds the ring near `VR_TARGET` by trimming the resample step
within ±1% (17 cents — inaudible). Consuming input slightly faster emits fewer samples per MP3
frame and drains a filling ring.

**Rules:**
- `VR_TARGET` must stay **below** `VR_SIZE - 1 - PF_AUDIO_HEADROOM` (1695 samples), or holding
  the target would itself be what keeps the prefetcher off. These two constants are coupled;
  move one and you must re-check the other.
- Servo on a low-passed level, never the instantaneous one: the ring swings by a whole chunk
  within one video frame, and servoing on that just modulates pitch at the frame rate.
- Reset the servo (`g_fill_ema`, `g_step`) wherever the ring is flushed — `video_audio_stop()`,
  which a seek goes through — or the empty ring reads as "starving" and the trim slams.

## Verifying on device

`g_show_debug` HUD, second line: `rd=` (blocking read ms), `pf=` (read hidden in the pacing
wait), `jpg=` (HW decode ms), `ring=` (servo error).

**`ring=` is the regression test for the drift bug.** Play a clip for 5+ minutes: it must sit
near 1200 the whole time. Climbing toward 4095 means the trim is not holding and the stutter is
coming back; falling to 0 is an underrun. `rd=` staying near 0 (with the work showing up in
`pf=`) means the prefetcher is alive — that is what the full ring used to destroy.

No host harness covers any of this: the JPEG peripheral, the SAI clock and the SD read latency
are all device-only. Test on hardware, with a long clip.
