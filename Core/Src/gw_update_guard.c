/* Pre-flight validation of the firmware update file: see gw_update_guard.h.
 *
 * The checks are chosen for what actually goes wrong with a file on an SD
 * card — truncation (partial download, interrupted copy) and garbage — not
 * for adversarial tampering:
 *
 *  - the updater blob's vector table must look like one (SP in RAM, PC a
 *    Thumb address in flash or RAM);
 *  - the embedded size field must fit its 1 MB slot;
 *  - the tar must start with "ustar" at its magic offset;
 *  - the tar must END with the two 512-byte zero blocks the format requires.
 *    A truncated file almost always cuts tar payload, so its last kilobyte
 *    is data, not zeros — this is the check that catches the classic brick.
 */
#include "gw_update_guard.h"

#include <stdio.h>
#include <string.h>

/* ustar magic sits at offset 257 of the first tar header block. */
#define TAR_MAGIC_OFFSET 257u
#define TAR_BLOCK        512u
#define TAR_END_ZEROS    (2u * TAR_BLOCK)

static bool sp_is_plausible(uint32_t sp)
{
    /* Any on-chip RAM an updater could stack in: DTCM, AXI/AHB SRAM, SRD. */
    return (sp >= 0x20000000u && sp <= 0x20020000u) ||
           (sp >= 0x24000000u && sp <= 0x240A0000u) ||
           (sp >= 0x30000000u && sp <= 0x30020000u);
}

static bool pc_is_plausible(uint32_t pc)
{
    if ((pc & 1u) == 0)  /* Cortex-M entry points are Thumb: bit 0 set */
        return false;
    return (pc >= 0x08000000u && pc <= 0x08200000u) ||  /* internal flash */
           (pc >= 0x20000000u && pc <= 0x20020000u) ||
           (pc >= 0x24000000u && pc <= 0x240A0000u) ||
           (pc >= 0x90000000u && pc <= 0x94000000u);    /* mapped ext flash */
}

update_guard_result_t update_guard_validate(update_guard_read_fn read_fn,
                                            void *ctx, uint32_t file_size)
{
    /* Header + size field + at least one tar block + the two end blocks. */
    if (file_size < UPDATE_GUARD_TAR_OFFSET + TAR_BLOCK + TAR_END_ZEROS)
        return UPDATE_GUARD_TOO_SMALL;

    uint32_t vectors[2];
    if (!read_fn(ctx, 0, vectors, sizeof(vectors)))
        return UPDATE_GUARD_READ_ERROR;
    if (!sp_is_plausible(vectors[0]) || !pc_is_plausible(vectors[1]))
        return UPDATE_GUARD_BAD_VECTORS;

    uint32_t updater_size;
    if (!read_fn(ctx, UPDATE_GUARD_HEAD_SIZE, &updater_size, 4))
        return UPDATE_GUARD_READ_ERROR;
    if (updater_size == 0 || updater_size > UPDATE_GUARD_HEAD_SIZE)
        return UPDATE_GUARD_BAD_SIZE_FIELD;

    char magic[5];
    if (!read_fn(ctx, UPDATE_GUARD_TAR_OFFSET + TAR_MAGIC_OFFSET, magic, 5))
        return UPDATE_GUARD_READ_ERROR;
    if (memcmp(magic, "ustar", 5) != 0)
        return UPDATE_GUARD_BAD_TAR_MAGIC;

    uint8_t tail[TAR_END_ZEROS];
    if (!read_fn(ctx, file_size - TAR_END_ZEROS, tail, TAR_END_ZEROS))
        return UPDATE_GUARD_READ_ERROR;
    for (uint32_t i = 0; i < TAR_END_ZEROS; i++) {
        if (tail[i] != 0)
            return UPDATE_GUARD_TRUNCATED;
    }

    return UPDATE_GUARD_OK;
}

const char *update_guard_reason(update_guard_result_t r)
{
    switch (r) {
    case UPDATE_GUARD_OK:             return "OK";
    case UPDATE_GUARD_TOO_SMALL:      return "file too small";
    case UPDATE_GUARD_BAD_VECTORS:    return "not an update image";
    case UPDATE_GUARD_BAD_SIZE_FIELD: return "corrupt header";
    case UPDATE_GUARD_BAD_TAR_MAGIC:  return "archive header missing";
    case UPDATE_GUARD_TRUNCATED:      return "file is cut short";
    default:                          return "read error";
    }
}

#ifndef UPDATE_GUARD_HOST_TEST
#include "ff.h"
#include "main.h"

static bool fatfs_read_at(void *ctx, uint32_t offset, void *buf, uint32_t len)
{
    FIL *f = (FIL *)ctx;
    UINT got = 0;
    wdog_refresh();
    if (f_lseek(f, offset) != FR_OK)
        return false;
    if (f_read(f, buf, len, &got) != FR_OK || got != len)
        return false;
    return true;
}

bool update_guard_check_and_quarantine(const char *path,
                                       update_guard_result_t *out_result)
{
    FIL f;
    update_guard_result_t r;

    if (f_open(&f, path, FA_READ) != FR_OK) {
        /* No file = nothing to guard; the caller already knows it exists,
         * so treat an open failure as a read error and do not reboot. */
        if (out_result)
            *out_result = UPDATE_GUARD_READ_ERROR;
        return false;
    }
    r = update_guard_validate(fatfs_read_at, &f, (uint32_t)f_size(&f));
    f_close(&f);

    if (out_result)
        *out_result = r;
    if (r == UPDATE_GUARD_OK)
        return true;

    /* Quarantine, loudly-by-name: a later cold boot must not burn this file
     * either. If a .bad already exists from a previous quarantine, replace
     * it so the rename cannot fail into a silent retry loop. */
    char bad_path[64];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    f_unlink(bad_path);
    f_rename(path, bad_path);
    return false;
}
#endif
