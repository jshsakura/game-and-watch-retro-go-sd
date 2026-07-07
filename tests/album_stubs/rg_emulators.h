#pragma once
#include <stddef.h>
/* the album only needs the shared-file arena accessors */
typedef struct { int _dummy; } retro_emulator_file_t;
retro_emulator_file_t *rg_emulators_shared_file_buffer(int *maxcount);
size_t rg_emulators_shared_file_bytes(void);
