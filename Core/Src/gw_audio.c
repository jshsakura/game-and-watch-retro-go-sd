#include "gw_audio.h"
#include <string.h>

uint32_t audio_mute;

int16_t audiobuffer_dma[AUDIO_BUFFER_LENGTH * 2] __attribute__((section(".audio")));

dma_transfer_state_t dma_state;
uint32_t dma_counter;

static uint16_t audiobuffer_full_length = AUDIO_BUFFER_LENGTH * 2;

// --- Music app: ISR-fed playback --------------------------------------------
// The fill routine (music_fill) lives HERE in the main firmware, so the SAI DMA
// ISR only ever runs CORE code — it never calls into the Music overlay (a stale
// overlay function pointer was the earlier black-screen brick). The decoded ring
// itself lives in the overlay (it's 16KB and won't fit in core RAM); the overlay
// registers it via music_attach(). music_fill only READS that buffer (RAM_EMU is
// always mapped) and is gated by music_owns (0 for emulators/launcher), so the
// worst case if the overlay were gone is a little noise — never a fault.
static int16_t           *m_ring;            // -> overlay ring (RAM_EMU)
static volatile uint16_t *m_head_p, *m_tail_p;
static int                m_mask;
static volatile int16_t   music_vol;
static volatile uint8_t   music_owns;        // 1 = Music app controls the DMA buffer
static volatile uint8_t   music_silent;      // 1 = output silence (paused / stopped)
static volatile uint32_t  music_played;

void     music_attach(int16_t *ring, int size, volatile uint16_t *head, volatile uint16_t *tail)
{ m_ring = ring; m_mask = size - 1; m_head_p = head; m_tail_p = tail; }
void     music_audio_enable(int on)          { music_owns = on ? 1 : 0; }
void     music_audio_set(int vol, int play)  { music_vol = (int16_t)vol; music_silent = play ? 0 : 1; }
void     music_audio_setpos(uint32_t p)      { music_played = p; }
uint32_t music_audio_pos(void)               { return music_played; }

// Fill the just-freed DMA half from the overlay ring — runs in the SAI ISR.
// No-op for emulators/launcher (music_owns == 0); clean silence when paused,
// stopped, or before the ring is attached.
static void music_fill(void)
{
    if (!music_owns) return;
    int16_t *buf = audio_get_active_buffer();
    int len = audio_get_buffer_length();
    if (music_silent || !m_ring) { memset(buf, 0, (size_t)len * sizeof(int16_t)); return; }
    int vol = music_vol, mask = m_mask;
    uint16_t tail = *m_tail_p, head = *m_head_p;
    for (int i = 0; i < len; i++) {
        int16_t s = 0;
        if (tail != head) { s = m_ring[tail]; tail = (tail + 1) & mask; }
        buf[i] = (int16_t)((s * vol) >> 8);
    }
    *m_tail_p = tail;
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
