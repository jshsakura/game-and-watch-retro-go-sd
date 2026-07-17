/* Rig stub for Core/Inc/main.h — gwenesis_bus.c includes main.h under
 * TARGET_GNW for the Ofast pragma; on the bare-metal QEMU rig we do NOT want
 * the STM32 HAL. Provide only what the core actually references. Mirrors the
 * tests/*_stubs/main.h pattern. */
#ifndef RIG_MD_MAIN_H
#define RIG_MD_MAIN_H
#include <stdint.h>
#include <stdbool.h>

/* NB: `uint` is provided as a macro by m68k.h — do not typedef it here. */

#define BOOT_MODE_APP  0
#define BOOT_MODE_WARM 1
#define BOOT_MODE_HOT  2

#ifndef __NOP
#define __NOP() do {} while (0)
#endif
#ifndef __WFI
#define __WFI() do {} while (0)
#endif

#endif
