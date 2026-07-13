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

/* ---------------------------------------------------------------------------
 * SM_STATE_VERSION is a promise a human has to remember to keep, and it was not
 * kept: the SPC player's serialized tail grew by 4 bytes (518b37b, "let
 * dsp_getSamples render mono") and the version stayed at 1. Yesterday's file
 * passed the magic/version check and the stream read four bytes long — exactly
 * the failure the header exists to prevent.
 *
 * The payload LENGTH is not a promise. It is a fact, and this build can measure
 * its own for free: stream a save into a counter that writes nothing. Two files
 * whose payloads differ in length cannot have the same layout, whoever forgot to
 * bump what. So the length is the check, and the version is now only a label.
 * ------------------------------------------------------------------------- */

/* What the pre-518b37b builds wrote. It is exactly SM_STATE_BYTES - 4, and those
 * four bytes are missing from the LAST segment in the stream (SpcPlayer). Every
 * segment before it — CPU, PPU, VRAM, cartridge SRAM, WRAM — is byte-identical
 * and lands where it belongs. So a v1 file is not corrupt: it is complete but for
 * the music player's final word, which we take as zero and load anyway rather
 * than throw away someone's game.
 *
 * A length that is neither this nor the current one is refused. A change anywhere
 * EARLIER in the stream shifts every segment after it, and that restores nonsense
 * with a straight face. */
#define SM_STATE_LEGACY_BYTES 276275u

typedef enum {
  SM_STATE_LOAD_OK,          /* this build's layout, load it */
  SM_STATE_LOAD_SHORT_TAIL,  /* the known older layout: load it, zero the tail */
  SM_STATE_LOAD_REFUSE,      /* some other layout, or truncated: do not touch the game */
} sm_state_load_t;

/** Decide before a single byte is streamed.
 *
 * @param hdr           the file's header
 * @param expected      what THIS build's save streams, counted at boot
 * @param payload_avail how many bytes actually follow the header on disk
 */
static inline sm_state_load_t sm_state_load_verdict(sm_state_header_t hdr,
                                                    uint32_t expected,
                                                    long payload_avail) {
  if (!sm_state_header_valid(hdr))
    return SM_STATE_LOAD_REFUSE;

  /* The file has to physically contain what its own header claims. RtlSaveLoadState
   * streams straight into WRAM, VRAM and the live PPU — there is nowhere to stage
   * 270 KB — so a file that runs out mid-stream has already destroyed the game it
   * was supposed to fall back to by the time anyone notices. */
  if (payload_avail < 0 || (uint32_t)payload_avail < hdr.bytes)
    return SM_STATE_LOAD_REFUSE;

  if (hdr.bytes == expected)
    return SM_STATE_LOAD_OK;

  if (hdr.bytes == SM_STATE_LEGACY_BYTES && expected == SM_STATE_LEGACY_BYTES + 4)
    return SM_STATE_LOAD_SHORT_TAIL;

  return SM_STATE_LOAD_REFUSE;
}
