/* See ram_diskio.h. Implements the five diskio glue functions FatFs expects,
 * plus get_fattime() (the firmware's ffconf.h has FF_FS_NORTC == 0).
 */
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"
#include "ram_diskio.h"

static unsigned char *g_disk;
static unsigned g_sectors;
static bool g_write_fails;

void ramdisk_create(unsigned sectors)
{
    free(g_disk);
    g_disk = calloc(sectors, RAMDISK_SECTOR_SIZE);
    g_sectors = g_disk ? sectors : 0;
    g_write_fails = false;
}

void ramdisk_destroy(void)
{
    free(g_disk);
    g_disk = NULL;
    g_sectors = 0;
    g_write_fails = false;
}

void ramdisk_arm_write_failure(void) { g_write_fails = true; }
void ramdisk_disarm_write_failure(void) { g_write_fails = false; }

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;
    return g_disk ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return g_disk ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!g_disk || sector + count > g_sectors)
        return RES_PARERR;
    memcpy(buff, g_disk + (size_t)sector * RAMDISK_SECTOR_SIZE,
           (size_t)count * RAMDISK_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!g_disk || sector + count > g_sectors)
        return RES_PARERR;
    if (g_write_fails)
        return RES_ERROR;
    memcpy(g_disk + (size_t)sector * RAMDISK_SECTOR_SIZE, buff,
           (size_t)count * RAMDISK_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    if (!g_disk)
        return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = g_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = RAMDISK_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* 2026-07-10 00:00:00, frozen so images are reproducible. */
DWORD get_fattime(void)
{
    return ((DWORD)(2026 - 1980) << 25) | ((DWORD)7 << 21) | ((DWORD)10 << 16);
}
