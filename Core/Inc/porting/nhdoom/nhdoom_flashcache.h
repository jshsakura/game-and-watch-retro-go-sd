/* nhdoom pre-baked flash-cache format (shared by the host baker and the device
 * loader).
 *
 * next-hack's engine builds a ~749KB runtime "flash cache" (composite textures,
 * level data, the lumpAddressTable) into FLASH_ADDRESS at boot. On the nRF52840
 * that target is writable internal flash; the Game & Watch (STM32H7B0) has no
 * spare writable flash, and the external OSPI flash that holds it cannot be
 * written while the engine executes XIP from it. So instead of letting the
 * device build the cache, we pre-build it ONCE on the host harness (from
 * doom1.mcu.wad) and ship the bytes. The engine still recomputes at boot, but
 * every storeWordToFlash() then sees *dest == word and is a no-op.
 *
 * The cache contains raw 32-bit absolute pointers (lump addresses into the WAD,
 * and pointers within the cache region) whose values depend on the runtime WAD
 * and cache base addresses. Those differ between the host bake and the device,
 * so the file carries a relocation table (mirrors the doom.ro sentinel patch in
 * rg_emulators.c). The device fixes each pointer by the WAD or cache base delta
 * as it copies the file into OSPI.
 *
 * File layout (all little-endian):
 *   [nhdoom_fc_header_t]                 at offset 0
 *   [cache_bytes of cache data]          at offset data_offset (4KB-aligned)
 *   [n_reloc * uint32_t reloc words]     at offset reloc_offset (4KB-aligned)
 *
 * data_offset is padded to FLASH_BLOCK_SIZE so the cache data starts on a flash
 * sector boundary (the device erases/programs it sector by sector), and
 * cache_bytes is rounded up to FLASH_BLOCK_SIZE so the reloc table never shares
 * a sector with cache data.
 *
 * Each reloc word packs the byte offset (into the cache data) in the high bits
 * and the pointer type in bit 0 (offsets are 4-byte aligned, so bit 0 is free):
 *   value = byte_offset | type,  type: 0 = WAD pointer, 1 = CACHE pointer.
 * Relocation (idempotent — only applied while the word is still in the host
 * range, so re-running on an already-patched OSPI copy is a no-op):
 *   *(cache + offset) += (type == CACHE) ? cache_delta : wad_delta;
 */
#ifndef NHDOOM_FLASHCACHE_H
#define NHDOOM_FLASHCACHE_H

#include <stdint.h>

#define NHDOOM_FC_MAGIC       0x4346484Eu   /* 'N','H','F','C' little-endian */
#define NHDOOM_FC_VERSION     1u
#define NHDOOM_FC_PAGE        4096u          /* == FLASH_BLOCK_SIZE           */

#define NHDOOM_FC_RELOC_WAD   0u
#define NHDOOM_FC_RELOC_CACHE 1u
#define NHDOOM_FC_TYPE_MASK   1u
#define NHDOOM_FC_OFFSET_MASK (~1u)

typedef struct {
    uint32_t magic;            /* NHDOOM_FC_MAGIC                            */
    uint32_t version;          /* NHDOOM_FC_VERSION                         */
    uint32_t cache_bytes;      /* cache data size, rounded up to PAGE       */
    uint32_t host_wad_base;    /* nh_ext_flash_base at bake time            */
    uint32_t host_wad_size;    /* nh_ext_flash_size at bake time            */
    uint32_t host_cache_base;  /* nh_flash_addr_base at bake time           */
    uint32_t host_cache_size;  /* FLASH_CACHE_REGION_SIZE at bake time      */
    uint32_t n_reloc;          /* number of reloc words                     */
    uint32_t data_offset;      /* file offset of cache data (PAGE-aligned)  */
    uint32_t reloc_offset;     /* file offset of reloc table (PAGE-aligned) */
} nhdoom_fc_header_t;

#endif /* NHDOOM_FLASHCACHE_H */
