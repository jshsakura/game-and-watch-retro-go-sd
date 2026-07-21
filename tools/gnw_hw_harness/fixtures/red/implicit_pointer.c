#include <stddef.h>

int main(void)
{
    void *p = ahb_malloc((size_t)64);
    return p == NULL;
}
