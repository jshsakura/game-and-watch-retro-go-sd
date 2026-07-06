/* POSIX directory shims for the FatFs stub. Isolated in its own translation
 * unit because <dirent.h> also typedefs `DIR`, which would clash with the
 * FatFs `DIR` in ff.h. This file therefore does NOT include ff.h. */
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    DIR *dir;
    char base[1024];
} pd_handle_t;

void *pd_opendir(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return NULL;

    pd_handle_t *h = calloc(1, sizeof(*h));
    if (!h) {
        closedir(d);
        return NULL;
    }
    h->dir = d;
    strncpy(h->base, path, sizeof(h->base) - 1);
    return h;
}

int pd_readdir(void *handle, char *name_out, size_t name_cap,
               int *is_dir, uint64_t *size_out)
{
    pd_handle_t *h = handle;
    struct dirent *de = readdir(h->dir);
    if (!de)
        return 0;

    strncpy(name_out, de->d_name, name_cap - 1);
    name_out[name_cap - 1] = '\0';

    char full[2048];
    snprintf(full, sizeof(full), "%s/%s", h->base, de->d_name);

    struct stat st;
    if (stat(full, &st) == 0) {
        *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        *size_out = (uint64_t)st.st_size;
    } else {
        *is_dir = 0;
        *size_out = 0;
    }
    return 1;
}

void pd_closedir(void *handle)
{
    pd_handle_t *h = handle;
    if (!h)
        return;
    if (h->dir)
        closedir(h->dir);
    free(h);
}
