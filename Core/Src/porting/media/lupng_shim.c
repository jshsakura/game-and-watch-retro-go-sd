// Builds the (unmodified, submodule) lupng.c with zlib->miniz name aliases.
//
// lupng.c lives in the retro-go-stm32 git submodule and is written against the
// zlib API, but this project's miniz.h omits the zlib-compatible aliases. We
// can't edit the submodule (CI checks out its pinned commit), so we compile it
// here through a shim that defines the aliases first. miniz.c is compiled
// separately, unaffected by these macros.
#include "miniz_zlib_compat.h"
#include "lupng.c"
