#ifndef _INCLUDE_SOUND_H
#define _INCLUDE_SOUND_H

/* CD-DA is 44.1kHz stereo on disc: running the mixer at 44100 lets it pass
 * through with no decimation filter (the old 22050 path halved the bandwidth
 * through a 4-tap low-pass), and the PSG renders natively at 44.1k. Costs a
 * little more PSG+mix work per frame; the DMA buffer fits (735*2 halfwords <=
 * AUDIO_BUFFER_LENGTH(1077)*2). Kept at 22050 there is no CD-DA path to feed. */
#define PCE_SAMPLE_RATE   (44100)

int  pce_snd_init(void);
void pce_snd_term(void);
void pce_snd_update(short *buffer, unsigned length);

#endif
