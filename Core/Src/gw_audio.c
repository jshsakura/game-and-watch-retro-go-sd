#include "gw_audio.h"
#include <string.h>

uint32_t audio_mute;

int16_t audiobuffer_dma[AUDIO_BUFFER_LENGTH * 2] __attribute__((section(".audio")));

dma_transfer_state_t dma_state;
uint32_t dma_counter;

static uint16_t audiobuffer_full_length = AUDIO_BUFFER_LENGTH * 2;

// --- Music app: ISR-fed playback ring ---------------------------------------
// The ring AND the fill routine live HERE in the main firmware, so the SAI DMA
// ISR only ever runs core code — it never calls into the Music overlay (a stale
// overlay function pointer was the earlier black-screen brick). The overlay just
// decodes into the ring via music_ring_push() and toggles music_on; emulators
// leave music_on = 0, so this is a no-op for them. Even if the overlay unloads
// while music_on is somehow set, music_fill only touches core RAM — worst case a
// little noise, never a fault.
#define MUSIC_RING_SIZE 8192
#define MUSIC_RING_MASK (MUSIC_RING_SIZE - 1)
static int16_t           music_ring[MUSIC_RING_SIZE];
static volatile uint16_t music_head, music_tail;   // SPSC: producer=head, consumer=tail
static volatile int16_t  music_vol;
static volatile uint8_t  music_owns;   // 1 = Music app controls the DMA buffer (else emu/launcher does)
static volatile uint8_t  music_silent; // 1 = output silence (paused / stopped)
static volatile uint32_t music_played;

void     music_ring_reset(void)              { music_head = music_tail = 0; }
int      music_ring_count(void)              { return (music_head - music_tail) & MUSIC_RING_MASK; }
void     music_ring_push(int16_t s)          { uint16_t n = (music_head + 1) & MUSIC_RING_MASK; if (n == music_tail) return; music_ring[music_head] = s; music_head = n; }
void     music_audio_enable(int on)          { music_owns = on ? 1 : 0; }
void     music_audio_set(int vol, int play)  { music_vol = (int16_t)vol; music_silent = play ? 0 : 1; }
void     music_audio_setpos(uint32_t p)      { music_played = p; }
uint32_t music_audio_pos(void)               { return music_played; }

// Fill the just-freed DMA half from the ring — runs in the SAI ISR. Untouched
// for emulators/launcher (music_owns == 0); clean silence when paused/stopped.
static void music_fill(void)
{
    if (!music_owns) return;
    int16_t *buf = audio_get_active_buffer();
    int len = audio_get_buffer_length();
    if (music_silent) { memset(buf, 0, (size_t)len * sizeof(int16_t)); return; }
    int vol = music_vol;
    uint16_t tail = music_tail, head = music_head;
    for (int i = 0; i < len; i++) {
        int16_t s = 0;
        if (tail != head) { s = music_ring[tail]; tail = (tail + 1) & MUSIC_RING_MASK; }
        buf[i] = (int16_t)((s * vol) >> 8);
    }
    music_tail = tail;
    music_played += len;
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai) {
    dma_counter++;
    dma_state = DMA_TRANSFER_STATE_HF;
    music_fill();
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai) {
    dma_counter++;
    dma_state = DMA_TRANSFER_STATE_TC;
    music_fill();
}

uint16_t audio_get_buffer_full_length() {
    return audiobuffer_full_length;
}

static void audio_set_buffer_full_length(uint16_t full_length) {
    audiobuffer_full_length = full_length;
}

// returns length of active buffer
uint16_t audio_get_buffer_length() {
    bool isFirstHalf = (dma_state == DMA_TRANSFER_STATE_HF) ? true : false;

    return isFirstHalf ? audiobuffer_full_length/2 : (audiobuffer_full_length+1)/2;
}

uint16_t audio_get_buffer_size() {
    return audio_get_buffer_length() * sizeof(int16_t);
}

int16_t *audio_get_active_buffer(void) {
    size_t offset = (dma_state == DMA_TRANSFER_STATE_HF) ? 0 : audiobuffer_full_length/2;

    return &audiobuffer_dma[offset];
}

int16_t *audio_get_inactive_buffer(void) {
    size_t offset = (dma_state == DMA_TRANSFER_STATE_TC) ? 0 : audiobuffer_full_length/2;

    return &audiobuffer_dma[offset];
}

void audio_clear_active_buffer() {
    bool isFirstHalf = (dma_state == DMA_TRANSFER_STATE_HF) ? true : false;

    memset(audio_get_active_buffer(), 0, (isFirstHalf ? audiobuffer_full_length/2 : (audiobuffer_full_length+1)/2) * sizeof(audiobuffer_dma[0]));
}

void audio_clear_inactive_buffer() {
    bool isFirstHalf = (dma_state == DMA_TRANSFER_STATE_HF) ? true : false;

    memset(audio_get_inactive_buffer(), 0, (isFirstHalf ? (audiobuffer_full_length+1)/2 : audiobuffer_full_length/2) * sizeof(audiobuffer_dma[0]));
}

void audio_clear_buffers() {
    memset(audiobuffer_dma, 0, sizeof(audiobuffer_dma));
}

void audio_start_playing(uint16_t length) {
    audio_start_playing_full_length(length*2);
}

void audio_start_playing_full_length(uint16_t full_length) {
    audio_clear_buffers();
    audio_set_buffer_full_length(full_length);
    HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t *) audiobuffer_dma, full_length);
}

void audio_stop_playing() {
    audio_clear_buffers();
    HAL_SAI_DMAStop(&hsai_BlockA1);
}
