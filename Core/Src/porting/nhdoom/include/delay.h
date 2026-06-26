/* nhdoom host port: shadow of src/delay.h. Host delay is a no-op (headless). */
#ifndef DELAY_H
#define DELAY_H
#include <stdint.h>
static inline void delay(uint32_t milliseconds) { (void)milliseconds; }
#endif
