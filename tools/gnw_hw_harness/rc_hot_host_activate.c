/* Host-only activation/coverage seam for the production SNES hot-RC objects.
 *
 * --wrap=snes_loadRom applies the same title/code-byte identity gates as
 * main_snes.c before calling the production rc_dispatch_init().
 * --wrap=rc_dispatch_lookup counts actual native hits and cold fallbacks.
 * No emulator behavior is implemented here. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "src/snes/snes.h"
#include "src/snes/cpu.h"
#include "src/snes/rc_dispatch.h"

#define RC_SMW_MAGIC 0x4D534352u
#define RC_SMW_TITLE_HASH 0xFB0BD0ECu

typedef void (*rc_smw_fn_t)(Cpu *);

extern const struct rc_smw_header {
  uint32_t magic;
  uint32_t nsites;
  uint32_t code_hash;
  const uint32_t *addrs;
  const rc_smw_fn_t *fns;
  const uint8_t *lens;
} rc_smw_header;

extern rc_entry_t rc_hash_storage[];
extern uint32_t rc_bank_off[];
extern uint32_t rc_bank_mask[];

bool __real_snes_loadRom(Snes *snes, const uint8_t *data, int length);
uint16_t __real_rc_dispatch_lookup(uint8_t bank, uint16_t pc);

static uint64_t lookup_hits;
static uint64_t lookup_misses;
static int candidate_active;

static uint32_t title_hash(const uint8_t *rom, uint32_t len) {
  if (!rom || len < 0x7FD5) return 0;
  uint32_t hash = 0x811C9DC5u;
  for (int i = 0; i < 21; i++) {
    hash ^= rom[0x7FC0 + i];
    hash *= 0x01000193u;
  }
  return hash;
}

static uint32_t translated_code_hash(const uint8_t *rom, uint32_t len) {
  uint32_t rom_mask = len - 1;
  uint32_t hash = 0x811C9DC5u;
  for (uint32_t i = 0; i < rc_smw_header.nsites; i++) {
    uint32_t address = rc_smw_header.addrs[i];
    uint8_t bank = (uint8_t)(address >> 16);
    uint16_t offset = (uint16_t)address;
    uint32_t index = ((uint32_t)(bank & 0x7F) << 15) | (offset & 0x7FFF);
    int nbytes = 1 + rc_smw_header.lens[i];
    for (int byte = 0; byte < nbytes; byte++) {
      hash ^= rom[(index + (uint32_t)byte) & rom_mask];
      hash *= 0x01000193u;
    }
  }
  return hash;
}

bool __wrap_snes_loadRom(Snes *snes, const uint8_t *data, int length) {
  bool loaded = __real_snes_loadRom(snes, data, length);
  rc_dispatch_reset();
  candidate_active = 0;
  if (!loaded) return false;

  uint32_t title = title_hash(data, (uint32_t)length);
  if (title != RC_SMW_TITLE_HASH) {
    fprintf(stderr,
            "[rc-validate] inactive title=%08x expected=%08x nsites=%u\n",
            title, RC_SMW_TITLE_HASH, rc_smw_header.nsites);
    return true;
  }
  if (rc_smw_header.magic != RC_SMW_MAGIC || rc_smw_header.nsites != 270) {
    fprintf(stderr, "[rc-validate] bad candidate magic=%08x nsites=%u\n",
            rc_smw_header.magic, rc_smw_header.nsites);
    return false;
  }

  uint32_t code_hash = translated_code_hash(data, (uint32_t)length);
  if (code_hash != rc_smw_header.code_hash) {
    fprintf(stderr,
            "[rc-validate] code hash mismatch got=%08x expected=%08x\n",
            code_hash, rc_smw_header.code_hash);
    return false;
  }

  rc_dispatch_init(rc_hash_storage, rc_bank_off, rc_bank_mask,
                   rc_smw_header.addrs, rc_smw_header.nsites,
                   (void (**)(Cpu *))rc_smw_header.fns);
  candidate_active = 1;
  fprintf(stderr, "[rc-validate] active nsites=%u codehash=%08x\n",
          rc_smw_header.nsites, code_hash);
  return true;
}

uint16_t __wrap_rc_dispatch_lookup(uint8_t bank, uint16_t pc) {
  uint16_t id = __real_rc_dispatch_lookup(bank, pc);
  if (id) lookup_hits++;
  else lookup_misses++;
  return id;
}

__attribute__((destructor)) static void report_coverage(void) {
  fprintf(stderr,
          "[rc-validate] active=%d native_hits=%llu cold_fallbacks=%llu\n",
          candidate_active, (unsigned long long)lookup_hits,
          (unsigned long long)lookup_misses);
}
