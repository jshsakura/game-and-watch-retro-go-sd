#include <stdio.h>
#include <stdlib.h>

#include "cps1_rom_linux.h"

static int read_file(const char *path, cps1_rom_region_t *out)
{
    if (!path) {
        out->data = NULL;
        out->size = 0;
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cps1_rom: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fprintf(stderr, "cps1_rom: '%s' is empty\n", path);
        fclose(f);
        return -1;
    }

    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "cps1_rom: short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    out->data = buf;
    out->size = (uint32_t)size;
    return 0;
}

int cps1_rom_load_linux(cps1_rom_t *rom, const char *prg_path, const char *gfx_path,
                         const char *z80_path, const char *oki_path)
{
    cps1_rom_region_t prg = {0}, gfx = {0}, z80 = {0}, oki = {0};

    if (read_file(prg_path, &prg) != 0) return -1;
    if (read_file(gfx_path, &gfx) != 0) return -1;
    if (read_file(z80_path, &z80) != 0) return -1;
    if (read_file(oki_path, &oki) != 0) return -1;

    return cps1_rom_attach(rom, prg, gfx, z80, oki);
}
