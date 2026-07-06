#pragma once
/* Host-test stub for main.h. rg_storage.c only needs wdog_refresh() from here.
 * SD_CARD is supplied on the compiler command line (-DSD_CARD=0/1). */
#include <stdint.h>

void wdog_refresh(void);
