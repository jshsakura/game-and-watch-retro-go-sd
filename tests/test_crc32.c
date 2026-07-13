/* Host unit test for Core/Src/porting/crc32.c against known CRC-32 (IEEE
 * 802.3 / zlib / PNG) test vectors. crc32_le(0, buf, len) is the standard
 * form: init(crc=~0), process, final(~crc) collapses to the textbook
 * init=0xFFFFFFFF/xorout=0xFFFFFFFF CRC-32 when the caller's seed is 0. */
#include <stdio.h>
#include <string.h>
#include "crc32.h"

static int failures = 0;
#define CHECK_EQ(got, want, label) do { \
    if ((got) != (want)) { failures++; printf("FAIL %s: got 0x%08x want 0x%08x\n", label, (unsigned)(got), (unsigned)(want)); } \
} while (0)

int main(void) {
    printf("=== crc32.c: known-vector pins ===\n");

    CHECK_EQ(crc32_le(0, (const unsigned char *)"", 0), 0x00000000u, "empty input");
    CHECK_EQ(crc32_le(0, (const unsigned char *)"123456789", 9), 0xcbf43926u, "\"123456789\" (standard CRC-32 check vector)");
    CHECK_EQ(crc32_le(0, (const unsigned char *)"a", 1), 0xe8b7be43u, "\"a\"");
    CHECK_EQ(crc32_le(0, (const unsigned char *)"abc", 3), 0x352441c2u, "\"abc\"");
    static const char pangram[] = "The quick brown fox jumps over the lazy dog";
    CHECK_EQ(crc32_le(0, (const unsigned char *)pangram, strlen(pangram)), 0x414fa339u, "pangram");

    /* Chained calls (seed = previous return) must equal one call over the
     * concatenation -- this is how the firmware streams a CRC across
     * multiple SD reads instead of buffering the whole file. */
    const char *a = "The quick brown fox ";
    const char *b = "jumps over the lazy dog";
    unsigned int chained = crc32_le(0, (const unsigned char *)a, strlen(a));
    chained = crc32_le(chained, (const unsigned char *)b, strlen(b));
    CHECK_EQ(chained, 0x414fa339u, "chained two-call == single-call over concatenation");

    /* A single flipped bit must change the checksum (sanity: not a
     * degenerate/constant table). */
    unsigned char corrupt[] = "abc"; corrupt[1] ^= 0x01;
    unsigned int c1 = crc32_le(0, (const unsigned char *)"abc", 3);
    unsigned int c2 = crc32_le(0, corrupt, 3);
    if (c1 == c2) { failures++; printf("FAIL single-bit flip did not change the CRC\n"); }

    if (failures) {
        printf("%d assertion(s) FAILED\n", failures);
        return 1;
    }
    printf("all crc32.c assertions passed\n");
    return 0;
}
