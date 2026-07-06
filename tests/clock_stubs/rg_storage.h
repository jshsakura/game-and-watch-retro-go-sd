#pragma once
/* host stub: the clock only uses rg_storage_mkdir ("/clock", "/clock/album"),
 * which is irrelevant to the logic under test — pretend it always succeeds. */
#include <stdbool.h>
static inline bool rg_storage_mkdir(const char *dir) { (void)dir; return true; }
