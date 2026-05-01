#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "rg_rtc.h"
#if SD_CARD == 1
#include "ff.h"
#else
#include "rg_frogfs.h"
#endif

#if SD_CARD == 1

extern uint32_t log_idx;
extern char logbuf[1024 * 4];

typedef struct {
    FIL file;
    int is_open;
} FatFSFile;

#define MAX_OPEN_FILES 10
FatFSFile file_table[MAX_OPEN_FILES];

void init_file_table() {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        file_table[i].is_open = 0;
    }
}

#define FATFS_FD_OFFSET 3 // Prevent collision with STDOUT_FILENO, ...
int find_free_slot() {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].is_open) {
            return i;
        }
    }
    return -1; // No free slot
}

int _open(const char *name, int flags, int mode)
{
    int slot = find_free_slot();
    if (slot == -1) {
        errno = ENFILE;
        return -1;
    }

    BYTE mode_flags = 0;
    switch (flags&0xFFFF) {
        case 0x0:       // "r"
            mode_flags = FA_READ;
            break;
        case 0x601:     // "w"
            mode_flags = FA_CREATE_ALWAYS | FA_WRITE;
            break;
        case 0x209:     // "a"
            mode_flags = FA_OPEN_APPEND | FA_WRITE;
            break;
        case 0x2:       // "r+"
            mode_flags = FA_READ | FA_WRITE;
            break;
        case 0x602:     // "w+"
            mode_flags = FA_CREATE_ALWAYS | FA_READ | FA_WRITE;
            break;
        case 0x20a:     // "a+"
            mode_flags = FA_OPEN_APPEND | FA_READ | FA_WRITE;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    FRESULT res = f_open(&file_table[slot].file, name, mode_flags);
    if (res != FR_OK) {
        errno = EIO;
        return -1;
    }

    file_table[slot].is_open = 1;
    return slot + FATFS_FD_OFFSET;
;
}

int _close(int file)
{
    file = file - FATFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    FRESULT res = f_close(&file_table[file].file);
    if (res != FR_OK) {
        errno = EIO;
        return -1;
    }

    file_table[file].is_open = 0;
    return 0;
}

int _write(int file, char *ptr, int len)
{
    if (file == STDOUT_FILENO || file == STDERR_FILENO)
    {
        uint32_t idx = log_idx;
        if (idx + len + 1 > sizeof(logbuf))
        {
            idx = 0;
        }

        memcpy(&logbuf[idx], ptr, len);
        idx += len;
        logbuf[idx] = '\0';

        log_idx = idx;

        return len;
    }
    else
    {
        file = file - FATFS_FD_OFFSET;
        if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
            errno = EBADF;
            return -1;
        }

        UINT bytes_written;
        FRESULT res = f_write(&file_table[file].file, ptr, len, &bytes_written);
        if (res != FR_OK) {
            errno = EIO;
            return -1;
        }

        return bytes_written;
    }
}

int _read(int file, char *ptr, int len)
{
    file = file - FATFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    UINT bytes_read;
    FRESULT res = f_read(&file_table[file].file, ptr, len, &bytes_read);
    if (res != FR_OK) {
        errno = EIO;
        return -1;
    }

    return bytes_read;
}

off_t _lseek(int file, off_t offset, int whence) {
    file = file - FATFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    DWORD new_offset;
    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = f_tell(&file_table[file].file) + offset;
            break;
        case SEEK_END:
            new_offset = f_size(&file_table[file].file) + offset;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    FRESULT res = f_lseek(&file_table[file].file, new_offset);
    if (res != FR_OK) {
        errno = EIO;
        return -1;
    }

    return new_offset;
}

static time_t fatfs_to_unix_timestamp(WORD fdate, WORD ftime) {
    struct tm t;

    // Extract date
    t.tm_year  = ((fdate >> 9) & 0x7F) + 80; // Years since 1900
    t.tm_mon   = ((fdate >> 5) & 0x0F) - 1;  // Month (0-11)
    t.tm_mday  = (fdate & 0x1F);             // Day (1-31)

    // Extract time
    t.tm_hour  = (ftime >> 11) & 0x1F;       // Hour (0-23)
    t.tm_min   = (ftime >> 5) & 0x3F;        // Minute (0-59)
    t.tm_sec   = (ftime & 0x1F) * 2;         // Second (0-59)

    t.tm_isdst = -1; // Daylight Saving Time flag

    return mktime(&t);
}

int _fstat(int file, struct stat *st) {
    file = file - FATFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    st->st_size = f_size(&file_table[file].file);
    st->st_mode = S_IFREG;
    // TODO : fill st->st_mtime if needed
    return 0;
}

int stat(const char *path, struct stat *st) {
    FILINFO fno;
    FRESULT res;

    // Use FatFs to get file information
    res = f_stat(path, &fno);
    if (res != FR_OK) {
        // Map FatFs error to errno
        switch (res) {
            case FR_NO_FILE:
            case FR_NO_PATH:
                errno = ENOENT;  // No such file or directory
                break;
            case FR_DENIED:
                errno = EACCES;  // Permission denied
                break;
            case FR_INVALID_NAME:
                errno = EINVAL;  // Invalid path
                break;
            default:
                errno = EIO;     // I/O error
                break;
        }
        return -1;
    }

    // Populate the stat structure
    st->st_size = fno.fsize;  // File size
    if (fno.fattrib & AM_DIR) {
        st->st_mode = S_IFDIR;  // Directory
    } else {
        st->st_mode = S_IFREG;  // Regular file
    }

    // Set modification time
    st->st_mtime = fatfs_to_unix_timestamp(fno.fdate, fno.ftime);

    return 0;
}

int _isatty(int file)
{
    return 0;
}

int _feof(int file) {
    file = file - FATFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    if (f_eof(&file_table[file].file)) {
        return 1;
    }
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    FRESULT res;

    res = f_mkdir(path);
    switch (res)
    {
        case FR_OK:
            return 0;
            break;
        case FR_EXIST:
            errno = EEXIST;
            break;
        case FR_NO_PATH:
            errno = ENOENT;
            break;
        case FR_INVALID_NAME:
            errno = EINVAL;
            break;
        case FR_DENIED:
            errno = EACCES;
            break;
        case FR_DISK_ERR:
            errno = EIO;
            break;
        default:
            errno = EIO;
            break;
    }
    return -1;
}

int rmdir(const char *path) {
    FRESULT res;
    DIR dir;
    FILINFO fno;

    // Open the directory
    res = f_opendir(&dir, path);
    if (res != FR_OK) {
        return -1;  // Directory could not be opened
    }

    // Check if the directory is empty before deleting
    res = f_readdir(&dir, &fno);
    if (res != FR_OK || fno.fname[0] != 0) {
        // Directory is not empty or an error occurred
        f_closedir(&dir);
        return -1;
    }

    // Remove the directory if empty
    f_closedir(&dir);
    res = f_unlink(path);
    
    if (res == FR_OK) {
        return 0;  // Success
    }
    
    return -1;  // Failure
}

int _unlink(const char *path) {
    FRESULT res;
    res = f_unlink(path);
    if (res == FR_OK) {
        return 0;
    } else {
        errno = ENOENT;
        return -1;
    }
}

int __wrap_fflush(int file) {
    file = file - FATFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    FRESULT res = f_sync(&file_table[file].file);
    if (res != FR_OK) {
        errno = EIO;
        return -1;
    }

    return 0;
}

#else

extern uint32_t log_idx;
extern char logbuf[1024 * 4];

typedef struct {
    frogfs_fh_t *file;
    int is_open;
} FrogFSFile;

#define MAX_OPEN_FILES 10
#define FROGFS_FD_OFFSET 3

static FrogFSFile file_table[MAX_OPEN_FILES];

void init_file_table(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        file_table[i].is_open = 0;
    }
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].is_open)
            return i;
    }
    return -1;
}

static const char *normalize_frogfs_path(const char *name, char *buffer, size_t buffer_size)
{
    if (!name || !buffer || buffer_size < 2)
        return NULL;

    if (name[0] == '/')
        return name;

    int written = snprintf(buffer, buffer_size, "/%s", name);
    if (written < 0 || (size_t)written >= buffer_size)
        return NULL;

    return buffer;
}

int _open(const char *name, int flags, int mode)
{
    (void)mode;

    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
        errno = EROFS;
        return -1;
    }

    frogfs_fs_t *fs = rg_frogfs_get();
    if (!fs) {
        errno = ENODEV;
        return -1;
    }

    char normalized_name[PATH_MAX];
    const char *frogfs_name = normalize_frogfs_path(name, normalized_name, sizeof(normalized_name));
    if (!frogfs_name) {
        errno = ENAMETOOLONG;
        return -1;
    }

    const frogfs_entry_t *entry = frogfs_get_entry(fs, frogfs_name);
    if (!entry || !frogfs_is_file(entry)) {
        errno = ENOENT;
        return -1;
    }

    int slot = find_free_slot();
    if (slot == -1) {
        errno = ENFILE;
        return -1;
    }

    frogfs_fh_t *file = frogfs_open(fs, entry, 0);
    if (!file) {
        errno = EIO;
        return -1;
    }

    file_table[slot].file = file;
    file_table[slot].is_open = 1;
    return slot + FROGFS_FD_OFFSET;
}

int _close(int file)
{
    file -= FROGFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    frogfs_close(file_table[file].file);
    file_table[file].file = NULL;
    file_table[file].is_open = 0;
    return 0;
}

int _write(int file, char *ptr, int len)
{
    if (file == STDOUT_FILENO || file == STDERR_FILENO)
    {
        uint32_t idx = log_idx;
        if (idx + len + 1 > sizeof(logbuf))
            idx = 0;

        memcpy(&logbuf[idx], ptr, len);
        idx += len;
        logbuf[idx] = '\0';

        log_idx = idx;
        return len;
    }

    errno = EROFS;
    return -1;
}

int _read(int file, char *ptr, int len)
{
    file -= FROGFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    return frogfs_read(file_table[file].file, ptr, len);
}

off_t _lseek(int file, off_t offset, int whence)
{
    file -= FROGFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    ssize_t pos = frogfs_seek(file_table[file].file, offset, whence);
    if (pos < 0) {
        errno = EINVAL;
        return -1;
    }

    return pos;
}

int _fstat(int file, struct stat *st)
{
    file -= FROGFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    ssize_t pos = frogfs_tell(file_table[file].file);
    ssize_t size = frogfs_seek(file_table[file].file, 0, SEEK_END);
    if (pos < 0 || size < 0) {
        errno = EIO;
        return -1;
    }
    frogfs_seek(file_table[file].file, pos, SEEK_SET);

    memset(st, 0, sizeof(*st));
    st->st_size = size;
    st->st_mode = S_IFREG;
    return 0;
}

int stat(const char *path, struct stat *st)
{
    frogfs_fs_t *fs = rg_frogfs_get();
    if (!fs) {
        errno = ENODEV;
        return -1;
    }

    const frogfs_entry_t *entry = frogfs_get_entry(fs, path);
    if (!entry) {
        errno = ENOENT;
        return -1;
    }

    frogfs_stat_t frog_stat;
    frogfs_stat(fs, entry, &frog_stat);

    memset(st, 0, sizeof(*st));
    st->st_size = frog_stat.size;
    st->st_mode = frogfs_is_dir(entry) ? S_IFDIR : S_IFREG;
    return 0;
}

int _feof(int file)
{
    file -= FROGFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    ssize_t pos = frogfs_tell(file_table[file].file);
    ssize_t size = frogfs_seek(file_table[file].file, 0, SEEK_END);
    if (pos < 0 || size < 0) {
        errno = EIO;
        return -1;
    }
    frogfs_seek(file_table[file].file, pos, SEEK_SET);
    return pos >= size;
}

int mkdir(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = EROFS;
    return -1;
}

int rmdir(const char *path)
{
    (void)path;
    errno = EROFS;
    return -1;
}

int _unlink(const char *path)
{
    (void)path;
    errno = EROFS;
    return -1;
}

int __wrap_fflush(int file)
{
    if (file == STDOUT_FILENO || file == STDERR_FILENO)
        return 0;

    file -= FROGFS_FD_OFFSET;
    if (file < 0 || file >= MAX_OPEN_FILES || !file_table[file].is_open) {
        errno = EBADF;
        return -1;
    }

    return 0;
}

#endif

int _gettimeofday(struct timeval *tv, void *tzvp)
{
    if (tv)
    {
        // get epoch UNIX time from RTC
        time_t unixTime = GW_GetUnixTime();
        tv->tv_sec = unixTime;

        // get millisecondes from rtc and convert them to microsecondes
        uint64_t millis = GW_GetCurrentMillis();
        tv->tv_usec = (millis % 1000) * 1000;
        return 0;
    }

    errno = EINVAL;
    return -1;
}
