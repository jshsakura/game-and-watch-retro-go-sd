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

/* min/max/constrain as templates, not macros. Arduino defines them as macros
 * and that is a landmine in a C++ translation unit: any libstdc++ header
 * included AFTER this one has its own std::min/std::max, and the preprocessor
 * rewrites those declarations into nonsense -- "macro 'min' passed 3 arguments,
 * but takes just 2" out of <bits/stl_algobase.h>, from a file that never
 * mentions min. It bit the moment this header was pulled into main_tamapoke.cpp,
 * which also reaches <limits>. Templates give the vendored code the same
 * spelling and the same behaviour with none of that.
 *
 * Deliberately NOT wrapped in #ifndef: a macro from elsewhere would still be
 * in force and would still break the same headers. If some other header defines
 * min as a macro first, that is a conflict to fix there, not to inherit here. */
template <typename T, typename U>
static inline auto min(T a, U b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template <typename T, typename U>
static inline auto max(T a, U b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
template <typename T, typename L, typename H>
static inline T constrain(T x, L lo, H hi) {
  return x < (T)lo ? (T)lo : (x > (T)hi ? (T)hi : x);
}

/* Upstream seeds from an unconnected ADC pin; on this hardware the RTC plus the
 * frame counter is the entropy we have, and the caller seeds explicitly. */
static inline long randomOf(long lo, long hi) { return lo + (rand() % (hi - lo)); }
static inline long random(long hi) { return rand() % (hi ? hi : 1); }
static inline long random(long lo, long hi) { return randomOf(lo, hi); }
static inline void randomSeed(unsigned long seed) { srand((unsigned)seed); }
