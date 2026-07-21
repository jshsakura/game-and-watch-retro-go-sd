/* Super Metroid: a savestate must restore what the RENDERER reads.
 *
 * Links the real external/sm/src/snes/ppu.c. The PPU keeps every screen-enable
 * register twice — once unpacked (`layer[i].mainScreenEnabled`) and once packed
 * (`screenEnabled[0]`, the byte the game wrote to $212C). ppu_saveload serializes
 * a byte range that ends at `pixelbuffer_placeholder`, and every packed copy sits
 * past that line. Only the packed copies are what PpuDrawBackgrounds() actually
 * consults:
 *
 *     #define IS_SCREEN_ENABLED(ppu, sub, layer) (ppu->screenEnabled[sub] & (1 << layer))
 *
 * So a savestate loaded into a freshly reset PPU — which is every "Resume game"
 * from the launcher, because that path boots the core and THEN loads — composites
 * no layers at all. Pure backdrop. A black screen that still runs at full speed
 * and near-zero CPU, because there is nothing to draw. That is the bug.
 *
 * The fix is not a format change: the data is already in the file, unpacked.
 * ppu_saveload has to rebuild its derived registers after a load, exactly as it
 * already rebuilds the palette. */

#include "types.h"
#include "snes/ppu.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* A savestate is a byte stream; this is the whole of one. */
static uint8 stream[512 * 1024];
static size_t stream_len, stream_pos;

static void stream_write(void *ctx, void *data, size_t size) {
    (void)ctx;
    memcpy(stream + stream_len, data, size);
    stream_len += size;
}

static void stream_read(void *ctx, void *data, size_t size) {
    (void)ctx;
    memcpy(data, stream + stream_pos, size);
    stream_pos += size;
}

/* The five registers the game writes to say "draw these layers". */
#define TM   0x2c   /* main screen enable   */
#define TS   0x2d   /* sub screen enable    */
#define TMW  0x2e   /* main screen window   */
#define TSW  0x2f   /* sub screen window    */
#define W12S 0x23   /* window mask, BG1/BG2 */

int main(void) {
    Ppu *ppu = ppu_init(NULL);

    /* A room in Super Metroid: BG1 + BG2 + sprites on the main screen, BG3 on the
     * sub screen for the colour-maths layer, windows on BG1. Nothing exotic —
     * just not zero. */
    ppu_reset(ppu);
    ppu_write(ppu, TM,   0x13);
    ppu_write(ppu, TS,   0x04);
    ppu_write(ppu, TMW,  0x01);
    ppu_write(ppu, TSW,  0x02);
    ppu_write(ppu, W12S, 0x03);

    CHECK(ppu->screenEnabled[0] == 0x13, "setup: TM did not reach screenEnabled[0]");

    uint8 want_tm  = ppu->screenEnabled[0];
    uint8 want_ts  = ppu->screenEnabled[1];
    uint8 want_tmw = ppu->screenWindowed[0];
    uint8 want_tsw = ppu->screenWindowed[1];
    uint32 want_ws = ppu->windowsel;

    stream_len = 0;
    ppu_saveload(ppu, &stream_write, NULL);

    /* The launcher's "Resume game" boots the core first and loads second, so the
     * PPU the state lands in is the one ppu_reset() just zeroed. Reproduce that
     * exactly — loading into the still-running PPU would hide the bug, which is
     * why it looked fine from the in-game menu. */
    ppu_reset(ppu);
    CHECK(ppu->screenEnabled[0] == 0, "a reset PPU must draw nothing (setup)");

    stream_pos = 0;
    ppu_saveload(ppu, &stream_read, NULL);

    CHECK(ppu->screenEnabled[0] == want_tm,
          "MAIN SCREEN LAYERS LOST: screenEnabled[0] = 0x%02X after load, saved 0x%02X. "
          "Zero means the compositor draws no layer at all — a black screen at full "
          "framerate, which is the reported symptom.",
          ppu->screenEnabled[0], want_tm);
    CHECK(ppu->screenEnabled[1] == want_ts,
          "sub screen layers lost: screenEnabled[1] = 0x%02X, saved 0x%02X",
          ppu->screenEnabled[1], want_ts);
    CHECK(ppu->screenWindowed[0] == want_tmw,
          "main screen windowing lost: 0x%02X, saved 0x%02X",
          ppu->screenWindowed[0], want_tmw);
    CHECK(ppu->screenWindowed[1] == want_tsw,
          "sub screen windowing lost: 0x%02X, saved 0x%02X",
          ppu->screenWindowed[1], want_tsw);
    CHECK(ppu->windowsel == want_ws,
          "window mask settings lost: 0x%06X, saved 0x%06X",
          (unsigned)ppu->windowsel, (unsigned)want_ws);

    /* And the caches that DO get invalidated must stay invalidated — the previous
     * fix in this same family, kept honest. */
    CHECK(ppu->paletteDirty, "the palette cache must be invalidated by a load");
    CHECK(ppu->lastBrightnessMult == 0xff,
          "the brightness table must be invalidated by a load");

    if (failures) {
        printf("FAIL test_sm_ppu_saveload: %d check(s) failed\n", failures);
        return 1;
    }
    printf("OK  test_sm_ppu_saveload: a load restores the registers the renderer reads\n");
    return 0;
}
