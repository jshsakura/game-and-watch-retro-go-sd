---
slug: jpeg-decoder-the-last-byte-and-the-eoi-that-never-came
title: 'JPEG decoder: the last byte, and the EOI that never came'
authors: [jshsakura]
tags: [video, fault, hardware]
image: /img/clock-hero.jpg
---

Nine frames out of ten were rejected. The HUD said `dec=14 v=272` — fourteen
decoded, two hundred and seventy-two rejected, on a clip that should have been
playing. The hardware JPEG decoder was stuck, and the stuck HAL lock we had
just fixed was real, but it was not *this*. This was something else, and it
was funnier, and smaller, and worse.

{/* truncate */}

This is the story of a one-byte bug in a hardware abstraction layer that
killed exactly one of three callers, while the other two swore the decoder
was fine.

## The peripheral

The STM32H7 has a hardware JPEG decoder. You feed it a JPEG stream, it
produces decoded pixels via DMA, it raises an interrupt when it hits the
end-of-image marker (`FF D9` — the EOI). The HAL function looks like:

```c
HAL_JPEG_Decode(hjpeg, InputBuffer, InDataLength, OutputBuffer, OutputLength, Timeout);
```

You pass it a buffer, a length, and a timeout. It returns when the decode is
done or the timeout fires. Simple.

We have three callers:

1. **The video player** — decodes one AVI chunk per frame. The chunk's
   payload is exactly one JPEG frame. The caller passes the chunk's exact
   length.
2. **The cover art cache** — decodes ROM cover thumbnails. The caller passes
   the cache slot's size, which is larger than the image (the slot is fixed,
   the image is variable).
3. **The `.gw` artwork reader** — decodes artwork embedded in a ROM file.
   The caller passes the rest of the ROM file from the image onward, which
   is much larger than the embedded JPEG.

Three callers. One decoder. One of them fails 95% of the time.

## The symptom

The video player's HUD showed `dec=14 v=272`. That is: out of 286 frames the
player tried to decode, 14 succeeded and 272 were rejected as "decode failed
or timed out." The clip was unwatchable — a still frame every second or so,
the rest black.

The HAL lock bug (a separate issue, where the decoder's mutex could get
stuck held) was already fixed. The stuck lock had masked this: with the lock
working, frames still failed, just for a different reason. The reason was in
the numbers. Fourteen out of two hundred and eighty-six is **4.9%**. About
one in twenty. That number is suspicious, because it is very close to "one in
sixteen," which is close to "one in four squared," which is the rate at which
a length-dependent bug would succeed if the length had to be a multiple of
four.

The frames whose length was a multiple of four succeeded. The rest didn't.

## The HAL line

`hal_jpeg.c`, line 1659:

```c
hjpeg->InDataLength = InDataLength - (InDataLength % 4UL);
```

The HAL **floors the input length to a multiple of four**. The hardware
decoder's DMA wants a length that is a multiple of four (it fetches 32-bit
words), so HAL rounds the length *down* to the nearest word boundary.

The 5,949-byte frame on screen was handed to the decoder as **5,948 bytes**.
The last byte — byte 5,949 — never arrived.

The last two bytes of any JPEG stream are `FF D9` — the **end-of-image
marker**. The decoder reads forward through Huffman tables and MCU blocks
until it sees `FF D9`, then raises the EOC (end-of-conversion) interrupt and
returns. Without the `FF D9`, the decoder never finishes. It sits there,
expecting more data, until the timeout fires and the caller reports failure.

A frame whose length is **already** a multiple of four loses zero bytes to
the floor, and the EOI arrives intact. A frame whose length is one, two, or
three bytes past a multiple of four loses its last byte — which is part of
the `FF D9` — and never terminates. About one frame in four has a length
that is a multiple of four. The 5% success rate was the rate at which the
floor happened to spare the EOI.

## Why only one caller suffered

Look at the three callers again:

- The **video player** passes the chunk's *exact* length. The floor bites
  into real image bytes. The decoder loses its EOI.
- The **cover art cache** passes the cache slot's size, which is larger than
  the image. The floor bites into bytes that are *past the EOI* — padding,
  not image. The decoder stops at the real EOI before ever reaching the
  truncated bytes. It succeeds.
- The **`.gw` artwork reader** passes the rest of the ROM file, which is
  much larger than the embedded JPEG. Same thing — the floor bites into
  bytes far past the image. The decoder succeeds.

Three callers. One passes an exact length. Two pass an over-long length.
The one with the exact length is the only one that can be hurt by the floor,
because it is the only one whose last byte *is* the image's last byte. The
other two's "last bytes" are rubbish the decoder never reads.

So the cover art and the `.gw` artwork both work perfectly. The video player
fails 95% of the time. And the cover-art code and the `.gw` code, written
independently of the video player, both *prove* — by working — that the
decoder is fine. They are not lying. The decoder is fine for them. It is the
*contract* between the video player and the HAL that is broken, and only one
side knows it.

## The fix

Round the length **up** instead of letting HAL round it down.

```c
/* HAL floors InDataLength to a multiple of 4, which truncates the FF D9 EOI
 * marker on frames whose length is not already word-aligned. Round up — the
 * extra bytes are still inside the caller's buffer (a frame slot is 64 KB;
 * a frame is not) and the decoder stops at EOI, so they are never read. */
size_t padded_len = (InDataLength + 3U) & ~3U;
HAL_JPEG_Decode(hjpeg, InputBuffer, padded_len, ...);
```

The caller's buffer is a 64 KB frame slot. The frame is at most a few tens
of KB. The bytes past the frame's real end, up to the next word boundary,
are inside the buffer — they are whatever was there before (usually zeroes
from the last clear). The decoder reads forward, hits the real `FF D9`,
stops. The extra one-to-three bytes are never decoded. The contract holds.

Every frame succeeds now. `dec=N v=0` on the HUD, for any N.

## The deeper bug: the tests were green

The CLAUDE.md for this project has a section called *"A test must compile
the file it claims to test."* It is about this file. The hardware JPEG
decoder had **three dedicated tests** and **0% coverage**. All three tests
reimplemented the HAL state machine in the test harness — they mocked the
hardware, wrote a software JPEG decoder that looked like the HAL from the
outside, and tested against that. The real `hw_jpeg_decoder.c` was never
linked into any test binary. Three device-killing bugs shipped from this
file while its tests were green.

The EOI bug was one of them. The HAL-lock bug was another. The third was an
input-size bug of the same shape — a different rounding, a different
truncation, same family. All three were in the contract between the driver
and the HAL. None of them were reachable from a test that mocked the HAL,
because the mock implemented the contract as the test author *thought* it
worked, not as the HAL actually worked.

The fix for the testing disease was `tools/jpeg_harness/run.sh` — a rig
that compiles the **actual** `hw_jpeg_decoder.c` (not a mock), links it
against a stubbed HAL, and feeds it real JPEG frames. The rig's design rule,
painted on the wall: *the test must link the same file the device links.*
And the rig's RED-before-GREEN rule: it must be demonstrable against the
pre-fix file, by checking it out of git history (`git show <hash>^:...`) and
showing it fail. A test that has never failed proves nothing.

## The lesson

The EOI bug taught me one thing I keep in my pocket:

**When one of three callers fails and the other two work, do not trust the
two that work.** They are not witnesses for the decoder. They are callers
with a more forgiving contract — a buffer larger than the image, a length
that over-runs the real data, a path that never exercises the boundary. The
bug is in the caller whose contract is tightest, and the other two's
success is the reason it took a week to find: every time you ask "is the
decoder broken?", the cover art loads fine and you conclude "no." The
decoder is fine. The decoder was always fine. The HAL is broken, and only
the caller that passes an exact length can see it.

The video player plays now. The decoder hits its EOI every frame. The cover
art still loads. The `.gw` artwork still loads. And the JPEG harness, built
the week after, links the real driver and fails loudly the moment anyone
reintroduces a truncation. Three tests at 0% coverage became one rig at
real coverage, and the next bug in that file will not ship green.
