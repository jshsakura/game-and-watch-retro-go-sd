/* Stand-in for the ESP32 Arduino core header.
 *
 * The vendored game logic (pet.cpp, i18n.cpp) includes this for little more
 * than the integer typedefs, millis() and Serial. Providing the header instead
 * of editing those files keeps them diffable against upstream.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tamapoke_shim.h"

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef constrain
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

/* Upstream seeds from an unconnected ADC pin; on this hardware the RTC plus the
 * frame counter is the entropy we have, and the caller seeds explicitly. */
static inline long randomOf(long lo, long hi) { return lo + (rand() % (hi - lo)); }
static inline long random(long hi) { return rand() % (hi ? hi : 1); }
static inline long random(long lo, long hi) { return randomOf(lo, hi); }
static inline void randomSeed(unsigned long seed) { srand((unsigned)seed); }
