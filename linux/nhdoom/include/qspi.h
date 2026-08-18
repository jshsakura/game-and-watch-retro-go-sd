/* nhdoom host port: shadow of src/qspi.h (only the two symbols the compiled
 * engine set references). External flash is the mmap'd WAD; programming is
 * a no-op on host. */
#ifndef QSPI_H
#define QSPI_H
#include <stdint.h>
uint32_t qspiFlashGetSize(void);
void qspiFlashProgram(uint32_t address, uint8_t *data, uint32_t size);
void qspiFlashErasePage4k(uint32_t address);
void qspiFlashErasePage64k(uint32_t address);
#endif
