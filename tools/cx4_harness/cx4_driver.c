/* Cx4 HLE driver — see run.sh. Compiles against the firmware's own
 * external/sm/src/snes/cx4_hle.c, never a copy of it: the harness that builds
 * its own version of the thing under test proves nothing, which this tree has
 * now learned from hw_jpeg_decoder.c and from tools/sm_harness alike.
 *
 * Every mode here is deterministic. There is no clock, no rand(), and the
 * cartridge image is generated from a fixed seed, so a hash is a hash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cx4_hle.h"

#define ROM_SIZE (512u * 1024u)

static uint8_t* g_rom;

/* xorshift32, fixed seed: the image has to be the same image on every machine
 * and every run, and it must not be all zeroes -- a zero ROM makes several
 * commands take their empty-list early exit and test nothing. */
static void rom_fill(uint32_t seed) {
    uint32_t s = seed;
    for (uint32_t i = 0; i < ROM_SIZE; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        g_rom[i] = (uint8_t)(s >> 24);
    }
}

/* The register file lives at RAM offset $1f40; the guest reaches RAM offset X
 * at bus address $6000+X. Writing $7f4f runs a command and $7f47 starts the
 * ROM DMA, so those two are only ever written deliberately. */
static void poke(Cx4* c, uint16_t off, uint8_t val) {
    cx4_write(c, (uint16_t)(0x6000 + off), val, g_rom, ROM_SIZE);
}

static void poke16(Cx4* c, uint16_t off, uint16_t val) {
    poke(c, off, (uint8_t)val);
    poke(c, (uint16_t)(off + 1), (uint8_t)(val >> 8));
}

static uint64_t hash_ram(const Cx4* c) {
    uint64_t h = 1469598103934665603ull;          /* FNV-1a 64 */
    for (uint32_t i = 0; i < sizeof(c->ram); i++) {
        h ^= c->ram[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Seed the whole register file from the same generator, so each command sees
 * plausible-but-fixed inputs rather than zeroes. */
static void seed_registers(Cx4* c, uint32_t seed) {
    uint32_t s = seed;
    for (uint16_t off = 0x1f40; off < 0x1fa0; off++) {
        if (off == 0x1f47 || off == 0x1f4f) continue;  /* trigger registers */
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        poke(c, off, (uint8_t)(s >> 24));
    }
}

static const uint8_t CMDS[] = {
    0x00, 0x01, 0x05, 0x0d, 0x10, 0x13, 0x15, 0x1f,
    0x22, 0x25, 0x2d, 0x40, 0x54, 0x5c, 0x89,
};

/* $7f4d selects among the seven sub-commands that share opcode $00. */
static const uint8_t MODES[] = { 0x00, 0x03, 0x05, 0x07, 0x08, 0x0b, 0x0c, 0x0e };

/* The regression sweep: every command, every sub-mode, one fixed seed each.
 * Prints one hash per step so a diff names the step that moved. */
static int mode_sweep(Cx4* c) {
    uint32_t seed = 0x12345678u;
    for (unsigned m = 0; m < sizeof(MODES); m++) {
        for (unsigned i = 0; i < sizeof(CMDS); i++) {
            cx4_init(c);
            seed_registers(c, seed);
            poke(c, 0x1f4d, MODES[m]);
            poke(c, 0x1f4f, CMDS[i]);           /* $7f4f: run it */
            printf("mode=%02x cmd=%02x %016llx\n",
                   MODES[m], CMDS[i], (unsigned long long)hash_ram(c));
            seed += 0x9e3779b9u;
        }
    }
    return 0;
}

/* $7f47 DMA with a length the cartridge is free to pick. $1f43 is 16 bits and
 * nothing in the encoding bounds it; the destination RAM is 8 KB. */
static int mode_overflow_dma(Cx4* c) {
    cx4_init(c);
    poke16(c, 0x1f40, 0x8000);        /* source, low 16 of a 24-bit address */
    poke(c, 0x1f42, 0x00);
    poke16(c, 0x1f45, 0x1f00);        /* destination, near the end of RAM    */
    poke16(c, 0x1f43, 0xffff);        /* length: 64 KB into 8 KB             */
    poke(c, 0x1f47, 0x00);            /* $7f47: go                           */
    printf("dma %016llx\n", (unsigned long long)hash_ram(c));
    return 0;
}

/* The two image commands size their clear from a width and a height that are
 * single register bytes: 255 x 255 / 2 asks for 32,512 bytes of an 8 KB RAM. */
static int mode_overflow_image(Cx4* c, uint8_t sub) {
    cx4_init(c);
    seed_registers(c, 0xfeedbeefu);
    poke(c, 0x1f89, 0xff);            /* width  */
    poke(c, 0x1f8c, 0xff);            /* height */
    poke(c, 0x1f4d, sub);
    poke(c, 0x1f4f, 0x00);
    printf("image sub=%02x %016llx\n", sub, (unsigned long long)hash_ram(c));
    return 0;
}

/* The wireframe chain-walk: an entry that is $ffff in both its endpoint pairs
 * satisfies a loop condition the loop body cannot change. */
static int mode_hang_wireframe(Cx4* c) {
    memset(g_rom, 0xff, ROM_SIZE);    /* every segment is a chain marker */
    cx4_init(c);
    poke(c, 0x295, 0x20);             /* 32 segments in the list */
    poke16(c, 0x1f80, 0x0000);        /* list at the start of the image */
    poke(c, 0x1f82, 0x00);
    poke(c, 0x1f4d, 0x08);
    poke(c, 0x1f4f, 0x00);
    printf("wireframe %016llx\n", (unsigned long long)hash_ram(c));
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s "
                "sweep|overflow-dma|overflow-scale|overflow-disint|hang-wireframe\n",
                argv[0]);
        return 2;
    }

    g_rom = malloc(ROM_SIZE);
    if (!g_rom) { fprintf(stderr, "rom alloc failed\n"); return 2; }
    rom_fill(0xc0ffee11u);

    /* malloc, not a stack or static buffer: ram[] is the last member, so a
     * write past it leaves the allocation and the sanitizer can see it. This
     * matches cart_attachCx4(), which callocs exactly sizeof(struct Cx4). */
    Cx4* c = malloc(sizeof(Cx4));
    if (!c) { fprintf(stderr, "cx4 alloc failed\n"); return 2; }
    memset(c, 0, sizeof(*c));

    int rc;
    if      (!strcmp(argv[1], "sweep"))           rc = mode_sweep(c);
    else if (!strcmp(argv[1], "overflow-dma"))    rc = mode_overflow_dma(c);
    else if (!strcmp(argv[1], "overflow-scale"))  rc = mode_overflow_image(c, 0x03);
    else if (!strcmp(argv[1], "overflow-disint")) rc = mode_overflow_image(c, 0x0b);
    else if (!strcmp(argv[1], "hang-wireframe"))  rc = mode_hang_wireframe(c);
    else { fprintf(stderr, "unknown mode: %s\n", argv[1]); rc = 2; }

    free(c);
    free(g_rom);
    return rc;
}
