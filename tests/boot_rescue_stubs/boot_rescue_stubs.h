/* Host-test seam for gw_boot_rescue.c: the three hardware touchpoints the
 * counter logic needs, provided as fakes by tests/test_boot_rescue.c. */
#ifndef BOOT_RESCUE_STUBS_H
#define BOOT_RESCUE_STUBS_H

#include <stdint.h>

uint32_t rescue_bkp_read(void);
void rescue_bkp_write(uint32_t value);
uint32_t rescue_now_ms(void);

/* Test-only introspection, defined in gw_boot_rescue.c under HOST_TEST. */
uint32_t boot_rescue_test_failed_boots(void);
void boot_rescue_test_reset_session(void);

#endif
