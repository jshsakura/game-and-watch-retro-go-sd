#pragma once
/* Host-test stub for <odroid_system.h>. rg_storage.c only needs the two path
 * helpers (real impl lives in rg_utils.c) plus the usual libc string decls. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h> /* strcasecmp used by rg_storage_get_adjacent_files */

const char *rg_basename(const char *path);
const char *rg_extension(const char *path);
