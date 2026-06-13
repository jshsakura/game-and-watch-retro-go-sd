/*
 * progjpeg.h - Minimal progressive-JPEG DC-scan decoder.
 *
 * Purpose: TJpgDec (and most embedded decoders) cannot decode progressive
 * JPEGs, and a full progressive decode needs the whole-image DCT coefficient
 * array (W/8 * H/8 * 64 * 2 bytes) which is far too much RAM for an STM32.
 *
 * For small album-art previews (shown at 112x112 / 34x34) we do not need a
 * full-resolution image. This decoder reads ONLY the first DC scan of a
 * progressive JPEG and reconstructs a 1/8-resolution image: each 8x8 DCT
 * block contributes exactly one pixel, derived from its DC coefficient
 * (the block average). The output is therefore ceil(W/8) x ceil(H/8) RGB888.
 *
 * Memory: no malloc, no libm. The caller supplies the output buffer. Input is
 * consumed strictly sequentially via a byte-source callback, so it can stream
 * from a file or from an MP3's embedded APIC region without buffering the
 * whole JPEG in RAM. The only large internal buffers are the per-component
 * DC planes (one byte per block), sized for PJ_MAX_BLOCKS_W/H.
 *
 * Limitations:
 *   - Progressive (SOF2) only. Baseline returns false (use TJpgDec for those).
 *   - Huffman coding only (arithmetic coding -> false).
 *   - Up to 3 components (grayscale or YCbCr). 4+ components -> false.
 *   - Decodes the first scan with Ss==0 && Se==0 (the first DC scan). DC
 *     refinement scans (Ah!=0) and all AC scans are ignored.
 */
#ifndef PROGJPEG_H
#define PROGJPEG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum 1/8-resolution dimensions we will reconstruct. 128 blocks bounds the
 * source at 1024x1024 (128*8) and the internal DC planes at 3*128*128 = 48 KB.
 * Album art shown at 112/34px needs far less; larger sources -> false (caller
 * falls back to the placeholder). */
#ifndef PJ_MAX_BLOCKS_W
#define PJ_MAX_BLOCKS_W 128
#endif
#ifndef PJ_MAX_BLOCKS_H
#define PJ_MAX_BLOCKS_H 128
#endif

/* Forward-only byte source. get() returns 0..255, or -1 at end of input.
 * It MUST NOT read past the logical end of the JPEG data (e.g. honour a
 * remaining-byte count when streaming from inside a larger file). */
typedef struct {
    int (*get)(void *user); /* returns next byte 0..255, or -1 on EOF */
    void *user;
} PjSource;

/* Decode the first DC scan of a progressive JPEG.
 *
 *   src       : byte source (see above)
 *   out       : caller buffer for interleaved RGB888, capacity out_cap bytes
 *   out_cap   : size of out in bytes
 *   out_w     : receives reconstructed width  (blocks wide, luma grid)
 *   out_h     : receives reconstructed height (blocks high, luma grid)
 *
 * Returns 1 on success, 0 on any malformed/unsupported/oversized input.
 * On success out holds out_w*out_h*3 bytes of RGB. */
int pj_decode_dc(PjSource *src, uint8_t *out, size_t out_cap,
                 int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* PROGJPEG_H */
