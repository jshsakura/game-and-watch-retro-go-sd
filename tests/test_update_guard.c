/* The update pre-flight gate, against synthetic update images in memory.
 *
 * This compiles the REAL Core/Src/gw_update_guard.c (validator half; the
 * FatFs wrapper is compiled out with -DUPDATE_GUARD_HOST_TEST). The images
 * follow scripts/gen_release_package.sh's real layout: updater blob padded
 * to 1 MB, u32 size field, then a ustar archive ending in two zero blocks.
 *
 * The case that matters most is TRUNCATION — the classic way a firmware
 * update bricks a unit is a partial copy burned as-is. A cut file's last
 * kilobyte is tar payload, not the end-of-archive zeros, and the validator
 * must see that.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_update_guard.h"

#define HEAD  UPDATE_GUARD_HEAD_SIZE
#define IMG_TAR_BLOCKS 4u   /* header + one data block + two zero end blocks */
#define IMG_SIZE (UPDATE_GUARD_TAR_OFFSET + IMG_TAR_BLOCKS * 512u)

static int failures = 0;

static void ok(bool cond, const char *what)
{
    printf("  %s %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) failures++;
}

typedef struct {
    const uint8_t *data;
    uint32_t size;
} image_t;

static bool mem_read(void *ctx, uint32_t offset, void *buf, uint32_t len)
{
    image_t *img = (image_t *)ctx;
    if (offset + len > img->size)
        return false;
    memcpy(buf, img->data + offset, len);
    return true;
}

/* A well-formed image, byte for byte like the release script makes one. */
static uint8_t *make_good_image(void)
{
    uint8_t *img = calloc(1, IMG_SIZE);
    uint32_t sp = 0x24080000u, pc = 0x08000101u, updater_size = 300 * 1024;

    memcpy(img + 0, &sp, 4);
    memcpy(img + 4, &pc, 4);
    memcpy(img + HEAD, &updater_size, 4);
    memcpy(img + UPDATE_GUARD_TAR_OFFSET + 257, "ustar", 5);
    memset(img + UPDATE_GUARD_TAR_OFFSET + 512, 0xAB, 512);  /* payload */
    /* last two 512-byte blocks stay zero: a valid end-of-archive */
    return img;
}

static update_guard_result_t run(const uint8_t *data, uint32_t size)
{
    image_t img = { data, size };
    return update_guard_validate(mem_read, &img, size);
}

int main(void)
{
    uint8_t *good = make_good_image();

    ok(run(good, IMG_SIZE) == UPDATE_GUARD_OK,
       "a well-formed image passes");

    /* Truncation: any cut that lands in payload must be caught — the tail
     * window then contains data bytes instead of the end-of-archive zeros.
     * Cutting 300 bytes leaves the 1 KB tail window straddling the 0xAB
     * payload block. */
    ok(run(good, IMG_SIZE - 300) == UPDATE_GUARD_TRUNCATED,
       "a file cut short is rejected as truncated");

    uint8_t *bad = make_good_image();
    memset(bad, 0xFF, 8);
    ok(run(bad, IMG_SIZE) == UPDATE_GUARD_BAD_VECTORS,
       "garbage where the updater's vectors should be is rejected");
    free(bad);

    bad = make_good_image();
    memset(bad + HEAD, 0, 4);
    ok(run(bad, IMG_SIZE) == UPDATE_GUARD_BAD_SIZE_FIELD,
       "a zero updater-size field is rejected");
    uint32_t huge = 2u * 1024 * 1024;
    memcpy(bad + HEAD, &huge, 4);
    ok(run(bad, IMG_SIZE) == UPDATE_GUARD_BAD_SIZE_FIELD,
       "an updater-size larger than its 1 MB slot is rejected");
    free(bad);

    bad = make_good_image();
    bad[UPDATE_GUARD_TAR_OFFSET + 257] = 'X';
    ok(run(bad, IMG_SIZE) == UPDATE_GUARD_BAD_TAR_MAGIC,
       "a missing ustar magic is rejected");
    free(bad);

    ok(run(good, 100 * 1024) == UPDATE_GUARD_TOO_SMALL,
       "a file smaller than the fixed header is rejected outright");

    /* Every rejection has words for the screen — the whole point is that
     * the user is told, not silently skipped. */
    bool all_named = true;
    for (int r = UPDATE_GUARD_TOO_SMALL; r <= UPDATE_GUARD_READ_ERROR; r++) {
        const char *s = update_guard_reason((update_guard_result_t)r);
        if (!s || !s[0] || strcmp(s, "OK") == 0)
            all_named = false;
    }
    ok(all_named, "every failure has an on-screen reason");

    free(good);
    printf(failures ? "test_update_guard: %d FAILED\n"
                    : "test_update_guard: all passed\n", failures);
    return failures ? 1 : 0;
}
