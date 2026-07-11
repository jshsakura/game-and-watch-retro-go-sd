#pragma once

#include <stddef.h>
#include <stdint.h>

int syscard_load_file(const char *path);
const char *syscard_find_default(void);
size_t syscard_get_data(unsigned char **data);
