#include <stdint.h>
#include <stdlib.h>

struct __attribute__((packed)) bad_layout {
    uint16_t prefix;
    uint8_t bytes[16];
};

int main(void)
{
    struct bad_layout *p = malloc(sizeof(*p));
    if (!p) return 2;
    volatile uint64_t *wide = (volatile uint64_t *)(void *)p->bytes;
    *wide = UINT64_C(0x1122334455667788);
    free(p);
    return 0;
}
