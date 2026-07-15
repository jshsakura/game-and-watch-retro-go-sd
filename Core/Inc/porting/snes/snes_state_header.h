#pragma once
/* Savestate stamp for the generic SNES core. A savestate is a raw dump of live
 * structs; a file this build did not write — a foreign magic, a stale layout,
 * or one truncated in transit — must be REFUSED, not restored into structs that
 * have since moved (project rule: a stale state "loads" and quietly restores
 * nonsense — a black screen with no clue why).
 *
 * Two defects that shipped from this exact code (commit 2f4f1343) are why the
 * checks are split the way they are, and why they live here where a host test
 * can drive them without the emulator core (tests/test_snes_state_header.c):
 *   1. the controller shift registers weren't in the stream — a length change,
 *      caught only because the payload length is a measured fact, not a promise;
 *   2. a truncated file was ACCEPTED — state_read zero-fills past EOF, so a
 *      short file "loaded" a half-zeroed machine and reported success. The size
 *      check below is what refuses it, BEFORE the machine is touched. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* SNES_STATE_VERSION goes up whenever the serialized layout moves. */
#define SNES_STATE_MAGIC    0x31534E53u   /* "SNS1" */
#define SNES_STATE_VERSION  2u            /* v2: + controller shift registers */

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t length;    /* payload bytes after this header */
} snes_state_header_t;

/* Foreign / stale-build stamp: refused before a single payload byte is read. */
static inline bool snes_state_header_valid(const snes_state_header_t *h) {
  return h->magic == SNES_STATE_MAGIC && h->version == SNES_STATE_VERSION;
}

/* The payload length is not a promise, it is a fact this build measures for
 * itself (stream a save into a counter that writes nothing). A file whose
 * header length disagrees, OR whose actual size isn't exactly header+payload,
 * cannot have this build's layout — refuse it. `expected` is the measured
 * length; `filesize` is the file's real byte count. */
static inline bool snes_state_payload_ok(const snes_state_header_t *h,
                                         uint32_t expected, long filesize) {
  return h->length == expected &&
         filesize == (long)(sizeof(snes_state_header_t) + expected);
}
