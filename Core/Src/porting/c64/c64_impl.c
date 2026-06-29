/* Single translation unit that compiles the floooh/chips Commodore 64 core
 * (header-only). c64.h does not self-include its dependencies, so they are
 * included here in order, then CHIPS_IMPL pulls in every implementation. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>   /* m6502.h uses a bare assert() in one unreached opcode slot */

#define CHIPS_IMPL
#define CHIPS_ASSERT(c) ((void)0)

#include "chips/chips_common.h"
#include "chips/m6502.h"
#include "chips/m6526.h"
#include "chips/m6522.h"
#include "chips/m6569.h"
#include "chips/m6581.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "chips/c1530.h"
#include "chips/c1541.h"
#include "chips/c64.h"
