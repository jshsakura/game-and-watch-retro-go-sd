#ifndef STUB_RG_EMULATORS_H
#define STUB_RG_EMULATORS_H
/* Minimal stub for the host clock preview: rg_clock_gif.c only needs the two
 * shared_files-buffer accessors (it uses the returned pointer as raw bytes). */
#include <stddef.h>
typedef struct { char _opaque; } retro_emulator_file_t;
retro_emulator_file_t *rg_emulators_shared_file_buffer(int *maxcount);
size_t rg_emulators_shared_file_bytes(void);
#endif
