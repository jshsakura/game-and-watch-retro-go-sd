// Exercise the real core ISR, including Music pause/volume and emulator ownership.
// Replace only HAL/DMA seams; no overlay code is called from the ISR.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "main.h"
#define _GW_AUDIO_H_
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_BUFFER_LENGTH 1077
typedef enum { DMA_TRANSFER_STATE_HF, DMA_TRANSFER_STATE_TC } dma_transfer_state_t;
typedef void (*emu_pull_fn_t)(int16_t *, uint16_t);
int16_t *audio_get_active_buffer(void);
uint16_t audio_get_buffer_length(void);
void audio_start_playing_full_length(uint16_t);
SAI_HandleTypeDef hsai_BlockA1;
static uint32_t tick;
uint32_t HAL_GetTick(void) { return tick; }
int HAL_SAI_Transmit_DMA(SAI_HandleTypeDef *s, uint8_t *p, uint16_t n)
{ (void)s; (void)p; (void)n; return 0; }
int HAL_SAI_DMAStop(SAI_HandleTypeDef *s) { (void)s; return 0; }
#include "../Core/Src/gw_audio.c"

static int pulls;
static void emu_pull(int16_t *p, uint16_t n) { memset(p, 0, n * sizeof *p); pulls++; }

int main(void)
{
    int16_t ring[4096];
    for (int i = 0; i < 4096; i++) ring[i] = 1234;
    volatile uint16_t head = 3000, tail = 0;
    uint32_t samples, edge;
    tick = 100;
    music_attach(ring, 4096, &head, &tail);
    audio_start_playing(AUDIO_BUFFER_LENGTH);
    music_audio_setpos(0);
    music_audio_enable(1);
    music_audio_set(256, 1);
    tick = 122;
    HAL_SAI_TxHalfCpltCallback(&hsai_BlockA1);
    music_audio_clock(&samples, &edge);
    assert(samples == 1077 && edge == 122 && tail == 1077);
    assert(audiobuffer_dma[0] == 1234);

    // Volume zero still advances time; pause does not consume ring or clock.
    music_audio_set(0, 1);
    tick = 144;
    HAL_SAI_TxCpltCallback(&hsai_BlockA1);
    music_audio_clock(&samples, &edge);
    assert(samples == 2154 && edge == 144 && tail == 2154);
    assert(audiobuffer_dma[1077] == 0);
    music_audio_set(256, 0);
    tick = 166;
    HAL_SAI_TxHalfCpltCallback(&hsai_BlockA1);
    music_audio_clock(&samples, &edge);
    assert(samples == 2154 && edge == 144 && tail == 2154);

    // Underrun silence still advances the physical output timeline.
    tail = head;
    music_audio_set(256, 1);
    music_audio_setpos(UINT32_MAX - 1000);
    tick = 188;
    HAL_SAI_TxCpltCallback(&hsai_BlockA1);
    music_audio_clock(&samples, &edge);
    assert(samples == 76 && edge == 188 && tail == head);

    // Emulator output must not depend on the Music clock.
    music_audio_enable(0);
    emu_audio_register(emu_pull);
    emu_audio_enable(1);
    tick = 210;
    HAL_SAI_TxHalfCpltCallback(&hsai_BlockA1);
    music_audio_clock(&samples, &edge);
    assert(pulls == 1 && samples == 76 && edge == 188);
    audio_start_playing(AUDIO_BUFFER_LENGTH);
    HAL_SAI_TxCpltCallback(&hsai_BlockA1);
    assert(pulls == 1);
    puts("OK real audio DMA clock: playback, mute, pause, underrun, wrap, emulator ownership");
}
