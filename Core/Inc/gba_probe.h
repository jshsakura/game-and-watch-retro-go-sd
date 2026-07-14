#ifndef _GBA_PROBE_H_
#define _GBA_PROBE_H_

#include <stdint.h>

/* Runs the OSPI XIP benchmark and halts, but only if GAME + TIME were held at
 * boot. Otherwise returns immediately and the console boots as usual. */
void gba_probe_run_if_requested(uint32_t boot_buttons);

#endif /* _GBA_PROBE_H_ */
