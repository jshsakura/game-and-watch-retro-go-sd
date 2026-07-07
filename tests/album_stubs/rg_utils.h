#pragma once
#include <string.h>
/* extension = text after the last '.', or NULL */
static inline const char *rg_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    return (dot && dot[1]) ? dot + 1 : (const char *)0;
}
