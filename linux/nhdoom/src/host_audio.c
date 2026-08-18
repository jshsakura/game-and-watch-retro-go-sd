/* nhdoom host port: replaces src/pwm_audio.c + Doom/source/i_audio.c.
 * Headless: no actual audio output, just satisfy the engine's sound API and
 * keep the per-channel bookkeeping s_sound.c reads back. */
#include <stdint.h>
#include "pwm_audio.h"

soundChannel_t soundChannels[MAX_CHANNELS];
int16_t        audioBuffer[AUDIO_BUFFER_LENGTH];

void initPwmAudio(void) {}
void updateSound(void) {}
void muteSound(void) {}

int  I_StartSound(int id, int channel, int vol, int sep)
{
    (void)sep;
    if (channel < 0 || channel >= MAX_CHANNELS) return -1;
    soundChannels[channel].lastAudioBufferIdx = 0xFFFF;
    soundChannels[channel].offset = 0;
    soundChannels[channel].sfxIdx = (uint8_t)id;
    soundChannels[channel].volume = (int8_t)vol;
    return channel;
}
void I_ShutdownSound(void) {}
void I_InitSound(void) {}
void I_InitMusic(void) {}
void I_PlaySong(int handle, int looping) { (void)handle; (void)looping; }
void I_PauseSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }
void I_StopSong(int handle) { (void)handle; }
void I_SetMusicVolume(int volume) { (void)volume; }
