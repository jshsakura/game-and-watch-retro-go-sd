/* The two linker symbols gw_flash_alloc.c reads as NUMBERS, not storage:
 *
 *   (uint32_t)&__EXTFLASH_BASE__    -> 0x90000000, where the flash is mapped
 *   (uint32_t)&__EXTFLASH_OFFSET__  -> bytes reserved at the bottom (--defsym)
 *
 * On the device the linker places them. Here we make their ADDRESSES be the
 * numbers we want, so the allocator computes exactly what it computes on ARM.
 */
#ifndef _TEST_GW_LINKER_H_
#define _TEST_GW_LINKER_H_

#include <stdint.h>

#define FAKE_EXTFLASH_BASE   0x90000000u
#define FAKE_EXTFLASH_RESERVED 0u          /* no OFW/chainloader reservation */

#define __EXTFLASH_BASE__   (*(uint8_t *)(uintptr_t)FAKE_EXTFLASH_BASE)
#define __EXTFLASH_OFFSET__ (*(uint8_t *)(uintptr_t)FAKE_EXTFLASH_RESERVED)

#endif
