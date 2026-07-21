#ifndef _TEST_FLASH_STUBS_H_
#define _TEST_FLASH_STUBS_H_
#include <stdint.h>
void fake_flash_create(uint32_t size);
void fake_flash_destroy(void);
const uint8_t *fake_flash_at(uint32_t offset);
#endif
