#pragma once
/* Host-test stub for the firmware config.h that rg_storage.h pulls in.
 * Only the two macros rg_storage.c / rg_storage.h actually touch are needed. */

#define RG_PATH_MAX 255

#ifndef RG_STORAGE_ROOT
/* Matches production (retro-go-stm32/components/odroid/config.h): empty root. */
#define RG_STORAGE_ROOT ""
#endif
