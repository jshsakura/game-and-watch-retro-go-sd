/* The G&W piezo ring must not be written past its end.
 *
 * gw_system_sound_melody() appends one sample per R-pin write and only rewinds
 * when the host marks the buffer copied. A long halt or bootstrap can write far
 * more samples than the ring holds before that happens, and on the device the
 * next BSS words are gw_audio_buffer_idx and the device_run/device_blit
 * function pointers: overwrite those and the next gw_system_run() jumps to 0.
 *
 * Compiles the real gw_system.c and the real SM5xx cores, with ASan, so a write
 * past gw_audio_buffer is a global-buffer-overflow report, not luck. Only the
 * ROM loader, graphics and button reader are stubbed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_type_defs.h"
#include "gw_romloader.h"
#include "gw_graphic.h"
#include "gw_system.h"

unsigned char *gw_program;
unsigned char *gw_melody;   /* NULL: the piezo path, not the SM511 melody one */
unsigned int *gw_keyboard;
gwromheader_t gw_head;

bool gw_romloader(void) { return true; }
void gw_gfx_init(void) {}
void gw_gfx_sm500_rendering(uint16 *framebuffer) { (void)framebuffer; }
void gw_gfx_sm510_rendering(uint16 *framebuffer) { (void)framebuffer; }
unsigned int gw_get_buttons(void) { return 0; }

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    /* Arrange: a fresh ring, piezo on R1, nobody draining it */
    memset(&gw_head, 0, sizeof(gw_head));
    gw_system_sound_init();

    /* Act: four rings' worth of samples before the host copies anything */
    const size_t writes = 4 * sizeof(gw_audio_buffer);
    for (size_t i = 0; i < writes; i++)
        gw_writeR(1);

    /* Assert: the ring is full of the samples that fit (ASan has already
     * aborted the run if any write landed past the end) */
    check(gw_audio_buffer[0] == 1, "first sample stored");
    check(gw_audio_buffer[sizeof(gw_audio_buffer) - 1] == 1, "last slot stored");

    /* The host drains one period; the next write must rewind, not run on */
    gw_audio_buffer_copied = true;
    gw_writeR(0);
    check(gw_audio_buffer[GW_AUDIO_BUFFER_LENGTH] == 0,
          "after a drain the next sample lands right after the carried-over half");

    if (failures) {
        printf("test_gw_piezo_ring: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_gw_piezo_ring: ok (%zu writes into a %zu-byte ring)\n",
           writes, sizeof(gw_audio_buffer));
    return 0;
}
