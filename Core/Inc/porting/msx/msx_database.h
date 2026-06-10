#ifndef _MSX_DATABASE_H_
#define _MSX_DATABASE_H_
#include <stdio.h>
#include <stdint.h>
#include "rg_emulators.h"

#define SHA1_SIZE 20
#define SHA1_COMPACT_SIZE 6 // SHA1 hash is trunked to 6 bytes

typedef struct {
    uint8_t sha1[SHA1_COMPACT_SIZE];
    uint8_t mapper;
    uint8_t button_profile;
    uint8_t ctrl_required;
} RomInfo;

int8_t msx_get_game_info(const retro_emulator_file_t *active_file, RomInfo *result);

#endif
