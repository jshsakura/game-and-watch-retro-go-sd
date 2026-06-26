/* nhdoom host port: shadow of src/printf.h.
 * Route the engine's printf/sprintf straight at the host libc (we want the
 * real init/log output) and keep the _putchar contract. */
#ifndef _PRINTF_H_
#define _PRINTF_H_
#include <stdio.h>
void _putchar(char character);
#endif
