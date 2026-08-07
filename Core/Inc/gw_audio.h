#ifndef _GW_AUDIO_H_
#define _GW_AUDIO_H_

#include "main.h"

extern SAI_HandleTypeDef hsai_BlockA1;
extern DMA_HandleTypeDef hdma_sai1_a;

// Default to 50Hz as it results in more samples than at 60Hz
#define AUDIO_SAMPLE_RATE   (48000)
// Must be large enough for any emulator's half-buffer.  Gwenesis PAL emits
// floor(313*3420/1008) = 1061 samples per frame (== GWENESIS_AUDIO_BUFFER_LENGTH_PAL),
// so the SAI/DMA half-buffer must hold at least 1061 (full buffer = 1061*2 = 2122).
// Use 1077 (== GWENESIS_AUDIO_BUFFER_CAPACITY) to match with a small margin.
#define AUDIO_BUFFER_LENGTH (1077)
extern uint32_t audio_mute;

typedef enum {
    DMA_TRANSFER_STATE_HF = 0x00,
    DMA_TRANSFER_STATE_TC = 0x01,
} dma_transfer_state_t;

extern int16_t audiobuffer_dma[AUDIO_BUFFER_LENGTH * 2] __attribute__((section (".audio")));
extern dma_transfer_state_t dma_state;
/* Written by the audio DMA half/full-transfer ISR (gw_audio.c) and read by
 * every core's frame-pacing spin. Without volatile the compiler may keep it
 * in a register across the loop, so the exit condition never re-reads memory
 * and the wait cannot end -- whether it does depends on inlining, which is why
 * this surfaced and vanished across builds that changed nothing nearby. */
extern volatile uint32_t dma_counter;

// Music app: ISR-fed playback. The fill routine lives in the main firmware
// (gw_audio.c) so the audio ISR never calls overlay code; the overlay owns the
// ring and registers it via music_attach().
void     music_attach(int16_t *ring, int size, volatile uint16_t *head, volatile uint16_t *tail);
void     music_audio_enable(int on);          // 1 = Music app owns the DMA buffer
void     music_audio_set(int vol, int play);  // play=0 -> ISR outputs silence
void     music_audio_setpos(uint32_t samples);
uint32_t music_audio_pos(void);

// Emulator ISR-fed playback. Same pattern as music_fill: the ISR calls ONLY
// core code (emu_fill below), which calls a pull function REGISTERED by the
// overlay. The overlay sets emu_owns=1 after registering and the core resets
// it to 0 in audio_start_playing so a stale pointer from a previous overlay
// can never fire. While emu_owns==1 the ISR pulls one DMA half-buffer per
// period from the overlay's ring — every period gets fresh audio regardless
// of main-loop timing, which eliminates the half-buffer resonance (only one
// half written at integer-ratio fps) and the double-emit pacing bug.
typedef void (*emu_pull_fn_t)(int16_t *dst, uint16_t n);
void     emu_audio_register(emu_pull_fn_t fn);
void     emu_audio_enable(int on);            // 1 = overlay pull owns the DMA buffer

uint16_t audio_get_buffer_full_length(void);
uint16_t audio_get_buffer_length(void);
uint16_t audio_get_buffer_size(void);
int16_t *audio_get_active_buffer(void);
int16_t *audio_get_inactive_buffer(void);
void audio_clear_active_buffer(void);
void audio_clear_inactive_buffer(void);
void audio_clear_buffers(void);
void audio_set_buffer_length(uint16_t length);
void audio_start_playing(uint16_t length);
void audio_start_playing_full_length(uint16_t length);
void audio_stop_playing(void);

#endif
