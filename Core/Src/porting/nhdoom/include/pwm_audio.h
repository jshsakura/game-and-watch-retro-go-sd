/* nhdoom host port: shadow of src/pwm_audio.h (types only; audio is stubbed). */
#ifndef SRC_PWM_AUDIO_H_
#define SRC_PWM_AUDIO_H_
#include <stdint.h>

#define MAX_CHANNELS 8
#define AUDIO_BUFFER_LENGTH 1024

typedef struct {
    uint16_t lastAudioBufferIdx;
    uint16_t offset;
    uint8_t  sfxIdx;
    int8_t   volume;
} soundChannel_t;

void initPwmAudio(void);
void updateSound(void);
void muteSound(void);
extern int16_t audioBuffer[AUDIO_BUFFER_LENGTH];
extern soundChannel_t soundChannels[MAX_CHANNELS];
#endif
