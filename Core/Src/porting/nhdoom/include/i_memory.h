/* nhdoom host port: shadow of Doom/include/i_memory.h.
 *
 * The engine compresses pointers two ways:
 *   - 16-bit "short" pointers for zone blocks (getShortPtr/getLongPtr): an
 *     offset within a single 256KB window based at RAM_PTR_BASE.
 *   - 3-byte "packed" addresses for column/sprite data (getPackedAddress):
 *     a region selector + 22-bit offset.
 * On the nRF52840 the three regions sit at fixed addresses (RAM 0x20000000,
 * external flash 0x12000000, internal flash 0).  On the host we instead point
 * the bases at the *actual* runtime locations: the linker pins the engine data
 * (incl. staticZone) in a 256KB-aligned window (nh_ram_window_base) and
 * host_main.c mmaps the WAD and the internal-flash cache, recording their
 * bases here.  The converted .mcu.wad stores only offsets (never packed device
 * addresses), so a self-consistent runtime packing is sufficient.
 */
#ifndef DOOM_INCLUDE_I_MEMORY_H_
#define DOOM_INCLUDE_I_MEMORY_H_
#include <stdint.h>

/* set once at startup by host_main.c (before Z_Init). */
extern unsigned long nh_ram_ptr_base;     /* 256KB-aligned base of engine data */
extern unsigned long nh_ext_flash_base;   /* mmap base of the WAD (XIP flash)  */
extern unsigned long nh_ext_flash_size;   /* WAD mapping length                */
extern unsigned long nh_flash_addr_base;  /* mmap base of internal-flash cache */

#define RAM_PTR_BASE   nh_ram_ptr_base
#define EXT_FLASH_BASE nh_ext_flash_base
#define FLASH_PTR_BASE 0
#define FLASH_ADDRESS  nh_flash_addr_base
#define WAD_ADDRESS    (EXT_FLASH_BASE + 4)

#define FLASH_CODE_KB 268
#define FLASH_IMMUTABLE_REGION_ADDRESS FLASH_ADDRESS
#define FLASH_IMMUTABLE_REGION 0
#define FLASH_LEVEL_REGION 1
#define FLASH_CACHE_REGION_SIZE ((1024 - FLASH_CODE_KB) * 1024)
#define FLASH_BLOCK_SIZE 4096
#define DEBUG_GETSHORTPTR 0
#define RAM_WINDOW_SIZE 0x40000UL   /* 256KB short-pointer reach */

/* range test: is this pointer in the mmap'd external flash (the WAD)? */
#define isOnExternalFlash(a) \
    (((uint32_t)(uintptr_t)(a) >= (uint32_t)nh_ext_flash_base) && \
     ((uint32_t)(uintptr_t)(a) <  (uint32_t)(nh_ext_flash_base + nh_ext_flash_size)))

typedef struct {
    uint8_t addrBytes[3];
} __attribute((packed)) packedAddress_t;

static inline void *getLongPtr(unsigned short shortPointer)
{
    if (!shortPointer) return 0;
    return (void *)(uintptr_t)(((unsigned int)shortPointer << 2) | RAM_PTR_BASE);
}
static inline unsigned short getShortPtr(void *longPtr)
{
    return (unsigned short)(((unsigned int)(uintptr_t)longPtr) >> 2);
}

/* 3-byte packing: selector in top 2 bits of byte[2], 22-bit offset-from-base.
 * Offset (not absolute) is stored so the runtime bases need no special
 * alignment; getPackedAddress / unpackAddress are exact inverses. */
static inline packedAddress_t getPackedAddress(void *addr)
{
    packedAddress_t ret;
    uint32_t a = (uint32_t)(uintptr_t)addr;
    if (!a) { ret.addrBytes[0] = ret.addrBytes[1] = ret.addrBytes[2] = 0; return ret; }
    uint32_t base; uint8_t sel;
    if (a >= nh_ext_flash_base && a < nh_ext_flash_base + nh_ext_flash_size) {
        base = nh_ext_flash_base; sel = 0x80;            /* external flash (WAD) */
    } else if (a >= nh_ram_ptr_base && a < nh_ram_ptr_base + RAM_WINDOW_SIZE) {
        base = nh_ram_ptr_base; sel = 0x00;              /* RAM                  */
    } else {
        base = nh_flash_addr_base; sel = 0xC0;           /* internal flash cache */
    }
    uint32_t off = a - base;
    ret.addrBytes[0] = (uint8_t)off;
    ret.addrBytes[1] = (uint8_t)(off >> 8);
    ret.addrBytes[2] = (uint8_t)(((off >> 16) & 0x3F) | sel);
    return ret;
}
static inline void *unpackAddress(packedAddress_t a)
{
    if (!a.addrBytes[2] && !a.addrBytes[1] && !a.addrBytes[0]) return 0;
    uint32_t off = a.addrBytes[0] | (a.addrBytes[1] << 8) | ((a.addrBytes[2] & 0x3F) << 16);
    uint8_t sel = a.addrBytes[2] >> 6;
    uint32_t base = (sel == 0x2) ? nh_ext_flash_base
                  : (sel == 0x3) ? nh_flash_addr_base
                  : nh_ram_ptr_base;
    return (void *)(uintptr_t)(base + off);
}

#endif
