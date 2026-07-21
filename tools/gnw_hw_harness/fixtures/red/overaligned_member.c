#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ABI_PTR_ALIGN __attribute__((aligned(8)))

struct window {
    uint16_t *content_tilemap ABI_PTR_ALIGN;
};

void scroll_window_up(struct window *w, size_t nbytes)
{
    memmove(w->content_tilemap, w->content_tilemap + 16, nbytes);
}
