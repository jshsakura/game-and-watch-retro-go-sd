#ifndef _TEST_CRC32_H_
#define _TEST_CRC32_H_
#include <stdint.h>
uint32_t crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
#endif
