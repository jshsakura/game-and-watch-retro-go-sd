/* nhdoom DEVICE port: replaces the nRF-specific bits of i_main.c (excluded).
 * The Game & Watch HAL_GetTick() millisecond counter drives the engine tic
 * timer; same arithmetic as upstream I_GetTime so demo/level timing matches.
 *
 * GPL-2.0 (see external/nh-doom, next-hack/nRF52840Doom lineage).
 */
#include <stdio.h>
#include <stdint.h>
#include "stm32h7xx_hal.h"      /* HAL_GetTick */
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "i_sound.h"
#include "global_data.h"

/* Microsecond clock from the 1 kHz SysTick (ms granularity is sufficient for
 * the ~28 ms TICRATE quantum). Kept as a separate function because several
 * engine call sites profile with it. */
unsigned int I_GetTimeMicrosecs(void)
{
    return (unsigned int)(HAL_GetTick() * 1000u);
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
