#ifndef _GW_UPDATE_GUARD_H_
#define _GW_UPDATE_GUARD_H_

#include <stdbool.h>
#include <stdint.h>

/* Pre-flight validation of /retro-go_update.bin BEFORE rebooting into the
 * bootloader that flashes it.
 *
 * The bootloader stage runs before any of our code and validates nothing: a
 * truncated download or an interrupted copy gets burned into bank 2 as-is,
 * and the resulting dark unit can only be recovered by draining the battery.
 * The firmware is the gate into that stage, so the firmware checks the file
 * first — and a bad file is NOT silently ignored: it is renamed to
 * `retro-go_update.bin.bad` (so a later cold boot cannot pick it up either)
 * and the user is told on screen, then the current firmware keeps running.
 *
 * File layout (scripts/gen_release_package.sh):
 *   [0]        firmware_update.bin (the updater app), zero-padded to 1 MB
 *   [1 MB]     u32 LE: real size of firmware_update.bin
 *   [1 MB + 4] gw_update.tar (ustar; ends with two 512-byte zero blocks)
 */

#define UPDATE_GUARD_HEAD_SIZE   (1024u * 1024u)
#define UPDATE_GUARD_TAR_OFFSET  (UPDATE_GUARD_HEAD_SIZE + 4u)

typedef enum {
    UPDATE_GUARD_OK = 0,
    UPDATE_GUARD_TOO_SMALL,      /* can't even hold header + one tar block   */
    UPDATE_GUARD_BAD_VECTORS,    /* updater's SP/PC are not plausible        */
    UPDATE_GUARD_BAD_SIZE_FIELD, /* embedded updater size is 0 or > 1 MB     */
    UPDATE_GUARD_BAD_TAR_MAGIC,  /* no "ustar" where the tar must start      */
    UPDATE_GUARD_TRUNCATED,      /* tar does not end in the two zero blocks  */
    UPDATE_GUARD_READ_ERROR,
} update_guard_result_t;

/* Read callback: fill buf with len bytes at offset; return true on success.
 * Injected so the validator is pure logic and runs on a host against a file
 * image in memory. */
typedef bool (*update_guard_read_fn)(void *ctx, uint32_t offset,
                                     void *buf, uint32_t len);

update_guard_result_t update_guard_validate(update_guard_read_fn read_fn,
                                            void *ctx, uint32_t file_size);

/* Short human-readable reason for the on-screen message. */
const char *update_guard_reason(update_guard_result_t r);

#ifndef UPDATE_GUARD_HOST_TEST
/* Device wrapper: validate the file at `path` on the mounted SD card.
 * Returns true when the update may proceed. On a bad file it renames it to
 * "<path>.bad" and returns false — the caller shows the alert. */
bool update_guard_check_and_quarantine(const char *path,
                                       update_guard_result_t *out_result);
#endif

#endif
