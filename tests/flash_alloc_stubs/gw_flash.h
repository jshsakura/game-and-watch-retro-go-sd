#ifndef _TEST_GW_FLASH_H_
#define _TEST_GW_FLASH_H_
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
void OSPI_EnableMemoryMappedMode(void);
void OSPI_DisableMemoryMappedMode(void);
bool OSPI_Erase(uint32_t *address, uint32_t *size, bool blocking);
void OSPI_Program(uint32_t address, const uint8_t *buffer, size_t buffer_size);
uint32_t OSPI_GetSmallestEraseSize(void);
uint32_t OSPI_GetFlashSize(void);
#endif
