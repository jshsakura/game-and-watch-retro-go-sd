/* Host-test stub for Core/Inc/porting/porting.h. crc32.c only needs
 * DEXTFLASH_ATTR (the real header's is also empty -- the real one pulls in
 * stm32h7xx_hal.h for everything else, which this test has no use for). */
#ifndef STUB_PORTING_H
#define STUB_PORTING_H
#define DEXTFLASH_ATTR
#endif
