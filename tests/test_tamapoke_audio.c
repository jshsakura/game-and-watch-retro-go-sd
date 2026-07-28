/* TamaPoke's sound: does the real code actually produce samples?
 *
 * This links Core/Src/porting/tamapoke/tamapoke_audio.cpp itself -- the file the
 * firmware compiles -- because a harness that reimplements the thing it tests
 * proves nothing (root CLAUDE.md, and this tree has paid for it more than once).
 *
 * What it pins is the contract the frame loop depends on: sfxPlay() arms an
 * effect, tamapoke_audio_fill() renders it into the buffer the loop hands to the
 * SAI DMA, silence is real silence rather than stale samples, and the effect stops
 * on its own. Every one of those is something the loop cannot check for itself:
 * it calls fill() unconditionally and plays whatever comes back.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tamapoke_audio.h"

#define N 4096

static int rc = 0;

static void ok(const char *m) { printf("  OK   %s\n", m); }
static void bad(const char *m) { printf("  FAIL %s\n", m); rc = 1; }

/* Peak absolute amplitude, and how many samples are non-zero. */
static void measure(const int16_t *b, int n, int *peak, int *nonzero)
{
    *peak = 0;
    *nonzero = 0;
    for (int i = 0; i < n; i++) {
        int v = b[i] < 0 ? -b[i] : b[i];
        if (v > *peak) *peak = v;
        if (b[i]) (*nonzero)++;
    }
}

int main(void)
{
    static int16_t buf[N];
    int peak, nz;

    printf("=== tamapoke: the sound path renders real samples ===\n");

    audioBegin();

    /* 1. With nothing playing, the buffer must be SILENCE -- and silence the
     *    function writes, not silence it leaves behind. The frame loop passes the
     *    same scratch buffer every frame; a fill() that returned without touching
     *    it would replay the last effect for ever. */
    memset(buf, 0x7F, sizeof(buf));
    tamapoke_audio_fill(buf, N);
    measure(buf, N, &peak, &nz);
    if (nz != 0)
        bad("an idle fill left non-zero samples -- the last effect would repeat");
    else
        ok("idle renders written silence, not stale samples");

    /* 2. An effect must actually sound. */
    sfxPlay(SFX_EAT);
    tamapoke_audio_fill(buf, N);
    measure(buf, N, &peak, &nz);
    if (peak < 1000)
        bad("sfxPlay(SFX_EAT) rendered nothing audible");
    else if (nz < N / 8)
        bad("sfxPlay(SFX_EAT) rendered mostly silence");
    else
        ok("an effect renders an audible square wave");

    /* 3. Every effect id, because a table with a wrong length is a table that
     *    reads past its own notes -- and SFX[] is indexed by the same enum the
     *    UI calls with. */
    int quiet = -1;
    for (int id = 0; id < SFX_COUNT; id++) {
        audioBegin();
        sfxPlay((uint8_t)id);
        tamapoke_audio_fill(buf, N);
        measure(buf, N, &peak, &nz);
        if (peak < 1000) { quiet = id; break; }
    }
    if (quiet >= 0) {
        char m[64];
        snprintf(m, sizeof(m), "effect id %d renders nothing", quiet);
        bad(m);
    } else {
        ok("all SFX_COUNT effects render");
    }

    /* 4. And it has to STOP. An effect that never clears leaves the DMA buzzing
     *    for the rest of the session. N_EAT is 102 ms at 16 kHz = ~1632 samples;
     *    two buffers of 4096 is far past it. */
    audioBegin();
    sfxPlay(SFX_EAT);
    tamapoke_audio_fill(buf, N);
    tamapoke_audio_fill(buf, N);
    measure(buf, N, &peak, &nz);
    if (nz != 0)
        bad("the effect was still sounding a whole buffer after it should have ended");
    else
        ok("an effect ends on its own");

    /* 5. The settings pill really mutes. audioSetEnabled(false) is what that
     *    screen toggles, and it has to reach the renderer, not just a flag. */
    audioBegin();
    audioSetEnabled(false);
    sfxPlay(SFX_HEART);
    tamapoke_audio_fill(buf, N);
    measure(buf, N, &peak, &nz);
    if (nz != 0)
        bad("sound plays with the setting turned off");
    else if (audioEnabled())
        bad("audioEnabled() disagrees with audioSetEnabled(false)");
    else
        ok("the sound setting silences the renderer");

    /* 6. And turning it back on works, or the pill is a one-way trip. */
    audioSetEnabled(true);
    sfxPlay(SFX_HEART);
    tamapoke_audio_fill(buf, N);
    measure(buf, N, &peak, &nz);
    if (peak < 1000)
        bad("sound does not come back after being re-enabled");
    else
        ok("re-enabling sound brings it back");

    printf("\n%s\n", rc ? "FAILED" : "PASSED");
    return rc;
}
