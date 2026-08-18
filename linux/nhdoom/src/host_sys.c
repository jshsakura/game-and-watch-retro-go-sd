/* nhdoom host port: replaces the nRF-specific bits of i_main.c (excluded).
 * Host monotonic clock drives the engine tic timer; same arithmetic as the
 * upstream I_GetTime so demo/level timing is unchanged. */
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "i_sound.h"
#include "global_data.h"

unsigned int I_GetTimeMicrosecs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)((uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u);
}

unsigned int I_GetTime(void)
{
    uint32_t time = I_GetTimeMicrosecs();
    if (!_g->basetime) { _g->basetime = time; return 0; }
    uint32_t diff = time - _g->basetime;
    diff = diff / (1000000 / TICRATE);
    return diff;
}

void I_Init(void)
{
    if (!(nomusicparm && nosfxparm))
        I_InitSound();
}

void _putchar(char c) { putchar(c); }
