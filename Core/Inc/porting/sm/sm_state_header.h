#pragma once
/* Savestate header for the Super Metroid port: a raw dump of live structs
 * outlives the firmware that wrote it. Change a struct's layout and yesterday's
 * file still opens, still reads to the end, and quietly restores nonsense —
 * which on this core means a black screen and no clue why. Stamp every file
 * with what wrote it, and refuse to load one this build did not.
 *
 * Pulled out of main_sm.c so a host test can drive sm_state_header_valid()
 * directly, against real fopen/fread streams, without linking the emulator
 * core main_sm.c sits on top of — see tests/test_sm_state_header.c. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* SM_STATE_VERSION goes up whenever the serialized layout moves. */
#define SM_STATE_MAGIC   0x314D5347u   /* "GSM1" */
#define SM_STATE_VERSION 1u

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t bytes;      /* payload length, so a truncated file is caught too */
} sm_state_header_t;

static inline bool sm_state_header_valid(sm_state_header_t hdr) {
  return hdr.magic == SM_STATE_MAGIC && hdr.version == SM_STATE_VERSION;
}

/* Reads a header from the current file position. False on a short read (file
 * truncated before a full header even landed) or a foreign one (wrong build) —
 * either way, *out is left holding whatever was actually on disk, so the caller
 * can still log the raw magic/version it refused. */
static inline bool sm_state_header_read(FILE *f, sm_state_header_t *out) {
  sm_state_header_t hdr = {0, 0, 0};
  bool got_full_header = fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
  *out = hdr;
  return got_full_header && sm_state_header_valid(hdr);
}
