#ifndef STUB_FAV_RG_EMULATORS_H
#define STUB_FAV_RG_EMULATORS_H
/* Minimal stand-in for the host favorites test — just enough of the real
 * Core/Inc/retro-go/rg_emulators.h shape for rg_favorites.c to compile and
 * link. Only pointer/opaque use of rom_system_t is needed (never
 * dereferenced by rg_favorites.c). */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef COVERFLOW
#define COVERFLOW 0
#endif

typedef enum
{
    REGION_NTSC = 0,
    REGION_PAL,
    REGION_SECAM,
    REGION_NTSC50,
    REGION_PAL60,
    REGION_AUTO
} rom_region_t;

typedef struct rom_system_t rom_system_t;

typedef struct {
    char name[256];
    const char *ext;
    char path[256];
    uint8_t *address;
    uint32_t size;
#if COVERFLOW != 0
    int img_state;
#endif
    rom_region_t region;
    const rom_system_t *system;
} retro_emulator_file_t;

typedef struct {
    char system_name[32];
    char dirname[16];
    char exts[32];
    struct {
        retro_emulator_file_t *files;
        int count;
        int maxcount;
    } roms;
    char browse_subpath[96];
    bool initialized;
    rom_system_t *system;
} retro_emulator_t;

retro_emulator_file_t *rg_emulators_shared_file_buffer(int *maxcount);
const rom_system_t *rg_emulators_system_for_dir(const char *dirname, size_t len);
bool emulator_show_file_menu(retro_emulator_file_t *file);
void emulator_show_file_info(retro_emulator_file_t *file);

#endif
