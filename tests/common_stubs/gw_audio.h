/* Host-test stub. dma_counter is the fake DMA-half clock common_emu_sound_sync()
 * waits on -- the test .c advances it (directly, or via __NOP()/__WFI() in
 * main.h) to make the busy-wait loop terminate deterministically. */
#ifndef STUB_GW_AUDIO_H
#define STUB_GW_AUDIO_H
#include <stdint.h>

extern uint32_t audio_mute;
extern uint32_t dma_counter;

void audio_clear_buffers(void);
void audio_clear_active_buffer(void);
void audio_stop_playing(void);

#endif
