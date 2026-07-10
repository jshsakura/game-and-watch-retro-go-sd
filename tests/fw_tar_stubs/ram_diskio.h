/* RAM-disk backend for the firmware-update tar harness (tests/test_fw_tar.c).
 *
 * Backs FatFs with a plain malloc'd sector array so tests can format a volume
 * of any size and drive extract_tar() to the exact failure the device hit.
 */
#ifndef RAM_DISKIO_H
#define RAM_DISKIO_H

#include <stdbool.h>
#include <stddef.h>

#define RAMDISK_SECTOR_SIZE 512

/* Allocate a zeroed volume of `sectors` x 512 bytes. Replaces any previous one. */
void ramdisk_create(unsigned sectors);
void ramdisk_destroy(void);

/* Once armed, every disk_write() returns RES_ERROR. Models a card that stops
 * accepting writes part-way through an update. */
void ramdisk_arm_write_failure(void);
void ramdisk_disarm_write_failure(void);

#endif /* RAM_DISKIO_H */
