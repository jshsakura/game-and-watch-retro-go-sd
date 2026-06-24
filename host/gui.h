#pragma once
#include <stdint.h>
typedef struct { uint16_t bg_c, main_c, sel_c, dis_c; } colors_t;
extern colors_t *curr_colors;
