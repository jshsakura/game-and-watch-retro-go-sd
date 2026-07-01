/* Single translation unit that compiles the floooh/chips ZX Spectrum core
 * (header-only). zx.h does not self-include its dependencies, so they are
 * included here in order, then CHIPS_IMPL pulls in every implementation. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define CHIPS_IMPL
#define CHIPS_ASSERT(c) ((void)0)

#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "chips/zx.h"
