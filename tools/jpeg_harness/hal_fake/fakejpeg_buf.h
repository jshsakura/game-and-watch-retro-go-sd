#ifndef FAKEJPEG_BUF_H
#define FAKEJPEG_BUF_H
#include <stdint.h>
#include <assert.h>

/* Fills buf[0..len) with non-0xFF filler, then writes an FF D9 marker ending
 * exactly at eoi_end (buf[eoi_end-2]=FF, buf[eoi_end-1]=D9), 2 <= eoi_end <=
 * len. Filler avoids 0xFF so no accidental marker precedes the real one. A
 * caller that wants "no EOI in range" (genuine truncation) passes eoi_end=0. */
static inline void fakejpeg_fill(uint8_t *buf, uint32_t len, uint32_t eoi_end)
{
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(0xAA + (i & 0x0F));
    if (eoi_end >= 2 && eoi_end <= len) {
        buf[eoi_end - 2] = 0xFF;
        buf[eoi_end - 1] = 0xD9;
    }
}

/* hw_jpeg_decoder.c addresses everything as uint32_t -- the device is
 * 32-bit. Test buffers are static (not heap/stack), and the harness links
 * -no-pie, so a normal x86-64 Linux load keeps their addresses under 4GB;
 * this turns a broken assumption into a loud assert instead of a silent
 * truncation. */
static inline uint32_t fakejpeg_addr(void *p)
{
    uintptr_t a = (uintptr_t)p;
    assert(a <= 0xFFFFFFFFu && "pointer does not fit uint32_t -- build with -no-pie");
    return (uint32_t)a;
}

#endif
