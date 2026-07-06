/* Fake FatFs directory API for host tests (SD_CARD=1).
 * Maps f_opendir/f_readdir/f_closedir onto a real temp directory through the
 * pd_* POSIX shims. Includes ff.h (FatFs DIR/FILINFO) but never <dirent.h>. */
#include "ff.h"
#include <string.h>

FRESULT f_opendir(DIR *dp, const TCHAR *path)
{
    if (!dp)
        return FR_INVALID_OBJECT;
    dp->impl = pd_opendir(path);
    if (!dp->impl)
        return FR_NO_PATH;
    return FR_OK;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno)
{
    if (!dp || !dp->impl || !fno)
        return FR_INVALID_OBJECT;

    memset(fno, 0, sizeof(*fno));

    int is_dir = 0;
    uint64_t size = 0;
    if (!pd_readdir(dp->impl, fno->fname, sizeof(fno->fname), &is_dir, &size)) {
        fno->fname[0] = '\0'; /* end-of-dir sentinel rg_storage.c checks */
        return FR_OK;
    }

    fno->fsize = (FSIZE_t)size;
    fno->fattrib = is_dir ? AM_DIR : 0;
    fno->ftime = 0;
    return FR_OK;
}

FRESULT f_closedir(DIR *dp)
{
    if (!dp)
        return FR_INVALID_OBJECT;
    pd_closedir(dp->impl);
    dp->impl = NULL;
    return FR_OK;
}
