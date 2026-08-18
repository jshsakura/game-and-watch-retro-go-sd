/* nhdoom host port: force-included before every TU.
 * Resolves the engine's `key_t` (uint16_t) vs glibc <sys/types.h> key_t clash
 * by claiming the glibc guard before any system header is seen. */
#ifndef NH_PRELUDE_H
#define NH_PRELUDE_H
#include <stdint.h>
#ifndef __key_t_defined
#define __key_t_defined
typedef uint16_t key_t;
#endif

/* Force our shadow i_memory.h (runtime pointer-packing bases) to win EVERYWHERE.
 * Engine headers under Doom/include quote-include "i_memory.h", which would
 * otherwise resolve to the original (compile-time RAM_PTR_BASE=0x20000000 etc.)
 * via the including-file-directory search before our -I path. Including it here
 * first claims the DOOM_INCLUDE_I_MEMORY_H_ guard so the original is skipped. */
#include "i_memory.h"
#endif
