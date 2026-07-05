#ifndef STUB_BITMAPS_H
#define STUB_BITMAPS_H
#include <stdint.h>
#define RG_LOGO_GNW 0
typedef struct { uint16_t width; uint16_t height; char logo[]; } retro_logo_image;
retro_logo_image *rg_get_logo(int16_t logo_index);
#endif
