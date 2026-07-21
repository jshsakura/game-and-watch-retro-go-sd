/* Host unit test for Core/Src/porting/lib/lz4_depack.c, against the
 * blz4-derived format lz4_depack() actually implements (see the file's own
 * header comment/credit -- it is not stock LZ4 block format). No compressor
 * exists in this tree to round-trip against, so the packed blocks below are
 * hand-built directly against the token grammar the decoder parses:
 *
 *   token = (lit_len << 4) | (match_len - 4)
 *   [extended lit_len bytes if lit_len==15]
 *   <lit_len literal bytes>
 *   -- if consuming the token's bytes reached packed_size: this was the
 *      final (literal-only) sequence, decoding ends here --
 *   <2-byte LE match offset>
 *   [extended match_len bytes if match_len==19]
 *
 * Boundary cases pinned: empty input (leading zero-byte sentinel), a block
 * that decompresses to exactly the destination's declared size (canary byte
 * immediately after the buffer must survive), a block with an actual
 * back-reference match, and a corrupt trailing sequence that the decoder's
 * own "last incomplete sequence" restriction checks must reject (return 0)
 * rather than accept as valid.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lz4_depack.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* dst has a canary byte right after the declared capacity -- any decoder
 * write past the intended output corrupts it, and we'd catch it here even
 * without ASan. */
#define CANARY 0xA5

static void test_empty_input(void) {
    unsigned char in[1] = { 0x00 }; /* leading zero byte: the decoder's own "not present" sentinel */
    unsigned char out[4] = {0};
    unsigned long n = lz4_depack(in, out, 1);
    CHECK(n == 0, "leading zero-byte input should decode to 0 bytes, got %lu", n);
}

static void test_literal_only_exact_bound(void) {
    /* token: lit_len=11 (fits the 4-bit nibble, no extension byte needed),
     * match nibble unused (this sequence ends the input, no match follows). */
    const char *msg = "HELLO WORLD";
    unsigned char in[1 + 11];
    in[0] = (unsigned char)(11 << 4);
    memcpy(in + 1, msg, 11);

    unsigned char out[11 + 1];
    out[11] = CANARY;
    unsigned long n = lz4_depack(in, out, sizeof(in));
    CHECK(n == 11, "expected 11 decompressed bytes, got %lu", n);
    CHECK(memcmp(out, msg, 11) == 0, "decompressed content mismatch");
    CHECK(out[11] == CANARY, "write past the exact-size output bound (canary clobbered)");
}

static void test_backreference_match(void) {
    /* "AB" literal + a match copying 4 bytes from offset 2 back, which (self-
     * referentially, LZ77-style) extends "AB" into "ABABAB". */
    unsigned char in[] = {
        (unsigned char)((2 << 4) | 0), /* lit_len=2, match_len=4 (nibble 0 + 4) */
        'A', 'B',
        0x02, 0x00, /* offset = 2, little-endian */
    };
    unsigned char out[6 + 1];
    out[6] = CANARY;
    unsigned long n = lz4_depack(in, out, sizeof(in));
    CHECK(n == 6, "expected 6 decompressed bytes, got %lu", n);
    CHECK(memcmp(out, "ABABAB", 6) == 0, "back-reference match produced wrong content: %.6s", out);
    CHECK(out[6] == CANARY, "write past the output bound (canary clobbered)");
}

static void test_corrupt_trailing_sequence_rejected(void) {
    /* Sequence 1: literal "ABCDE" (5) + a match (offset=3, len=4) that pads
     * dst_size up to 9, so the file's own restriction check
     * (dst_size >= 5 && lit_len < 5 on the FINAL sequence) is armed.
     * Sequence 2 (final, malformed): lit_len=2 < 5 with dst_size=9 >= 5 --
     * exactly the corrupt/truncated-encoding shape the decoder's own
     * end-of-block validation exists to catch. */
    unsigned char in[] = {
        (unsigned char)((5 << 4) | 0), 'A', 'B', 'C', 'D', 'E', 0x03, 0x00,
        (unsigned char)((2 << 4) | 0), 'X', 'Y',
    };
    unsigned char out[32];
    memset(out, CANARY, sizeof(out));
    unsigned long n = lz4_depack(in, out, sizeof(in));
    CHECK(n == 0, "malformed final sequence (dst_size>=5, trailing lit_len<5) should be rejected, got n=%lu", n);
}

int main(void) {
    printf("=== lz4_depack.c: round-trip + boundary cases ===\n");
    test_empty_input();
    test_literal_only_exact_bound();
    test_backreference_match();
    test_corrupt_trailing_sequence_rejected();

    if (failures) {
        printf("%d assertion(s) FAILED\n", failures);
        return 1;
    }
    printf("all lz4_depack.c assertions passed\n");
    return 0;
}
