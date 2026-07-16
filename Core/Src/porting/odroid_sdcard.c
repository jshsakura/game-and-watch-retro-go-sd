#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include "odroid_system.h"
#include "odroid_sdcard.h"

#define SDCARD_ACCESS_BEGIN()
#define SDCARD_ACCESS_END()

int odroid_sdcard_open()
{
    return 0;
}

int odroid_sdcard_close()
{
    return 0;
}

int odroid_sdcard_read_file(const char* path, void* buf, size_t buf_size)
{
    SDCARD_ACCESS_BEGIN();

    int count = -1;
    FILE* f;

    if ((f = fopen(path, "rb")))
    {
        count = fread(buf, 1, buf_size, f);
        fclose(f);
    }
    else
        printf("sd open fail: %s\n", path);

    SDCARD_ACCESS_END();

    return count;
}

int odroid_sdcard_write_file(const char* path, void* buf, size_t buf_size)
{
    SDCARD_ACCESS_BEGIN();

    int count = -1;
    FILE* f;

    if ((f = fopen(path, "wb")))
    {
        count = fwrite(buf, 1, buf_size, f);
        fclose(f);
    }
    else
        printf("sd open fail: %s\n", path);

    SDCARD_ACCESS_END();

    return count;
}

int odroid_sdcard_unlink(const char* path)
{
    SDCARD_ACCESS_BEGIN();

    int ret = unlink(path);

    SDCARD_ACCESS_END();

    return ret;
}

int odroid_sdcard_mkdir(const char *dir)
{
    SDCARD_ACCESS_BEGIN();

    int ret = mkdir(dir, 0777);

    if (ret == -1)
    {
        if (errno == EEXIST)
        {
            printf("odroid_sdcard_mkdir: Folder exists %s\n", dir);
            return 0;
        }

        char temp[255];
        strncpy(temp, dir, sizeof(temp) - 1);

        for (char *p = temp + strlen(ODROID_BASE_PATH) + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                if (strlen(temp) > 0) {
                    printf("odroid_sdcard_mkdir: Creating %s\n", temp);
                    mkdir(temp, 0777);
                }
                *p = '/';
                while (*(p+1) == '/') p++;
            }
        }

        ret = mkdir(temp, 0777);
    }

    if (ret == 0)
    {
        printf("odroid_sdcard_mkdir: Folder created %s\n", dir);
    }

    SDCARD_ACCESS_END();

    return ret;
}

int odroid_sdcard_get_filesize(const char* path)
{
    SDCARD_ACCESS_BEGIN();

    int ret = -1;
    FILE* f;

    if ((f = fopen(path, "rb")))
    {
        fseek(f, 0, SEEK_END);
        ret = ftell(f);
        fclose(f);
    }
    else
        printf("sd fsize fail: %s\n", path);

    SDCARD_ACCESS_END();

    return ret;
}

const char* odroid_sdcard_get_filename(const char* path)
{
    const char *name = strrchr(path, '/');
    return name ? name + 1 : NULL;
}

const char* odroid_sdcard_get_extension(const char* path)
{
    const char *ext = strrchr(path, '.');
    return ext ? ext + 1 : NULL;
}
