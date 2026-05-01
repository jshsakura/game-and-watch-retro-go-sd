#pragma once

#if SD_CARD == 0

#include <stdbool.h>
#include <stdint.h>
#include "frogfs/frogfs.h"

frogfs_fs_t *rg_frogfs_get(void);
bool rg_frogfs_get_file_data(const char *path, const uint8_t **data, uint32_t *size);

#endif
