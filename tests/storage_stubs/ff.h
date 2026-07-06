#pragma once
/* Minimal FatFs (ff.h) stub for host tests of rg_storage.c (SD_CARD=1).
 * Only the directory-iteration subset rg_storage.c actually calls is provided:
 * f_opendir / f_readdir / f_closedir + the FILINFO / DIR / FRESULT types.
 * Backed by a real temp directory via POSIX (see fake_fatfs.c / posix_dir.c).
 *
 * NOTE: the concrete f_* implementation lives in fake_fatfs.c, which must NOT
 * also include <dirent.h> (POSIX also names a type DIR). POSIX access is
 * isolated in posix_dir.c behind the pd_* shims declared here. */
#include <stdint.h>
#include <stddef.h>

typedef char TCHAR;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint64_t FSIZE_t;

typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_INVALID_OBJECT = 9,
    FR_INVALID_PARAMETER = 19,
} FRESULT;

/* File attribute bit used by rg_storage.c */
#define AM_DIR 0x10

#define FF_MAX_LFN 255

typedef struct {
    FSIZE_t fsize;
    WORD    fdate;
    WORD    ftime;
    BYTE    fattrib;
    TCHAR   fname[FF_MAX_LFN + 1];
} FILINFO;

typedef struct {
    void *impl;               /* opaque POSIX directory handle (pd_*) */
} DIR;

FRESULT f_opendir(DIR *dp, const TCHAR *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);

/* POSIX shims implemented in posix_dir.c (kept free of the FatFs DIR type). */
void *pd_opendir(const char *path);
/* Fills name/is_dir/size for the next entry. Returns 1 on entry, 0 at end. */
int   pd_readdir(void *h, char *name_out, size_t name_cap,
                 int *is_dir, uint64_t *size_out);
void  pd_closedir(void *h);
