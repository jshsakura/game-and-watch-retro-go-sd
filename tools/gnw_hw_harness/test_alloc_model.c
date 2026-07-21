#include "alloc_model.h"

#include <stdio.h>

#define DTCM_TOTAL 82944u
#define DTCM_INIT_USED 74728u
#define AHB_ALLOCATABLE 122880u
#define AHB_OLD_GBA_STATIC 43200u
#define RC_REQUEST (60u * 1024u)
#define DRAW2FB_REQUEST (84u * 1024u)

static uint8_t dtcm[DTCM_TOTAL];
static uint8_t ahb[AHB_ALLOCATABLE];
static int failures;

static void check(int condition, const char *message)
{
    printf("  %s %s\n", condition ? "OK  " : "FAIL", message);
    if (!condition) failures++;
}

int main(void)
{
    gnw_alloc_pool_t pool;
    printf("=== GNW constrained allocator regressions ===\n");

    check(gnw_alloc_pool_init(&pool, "dtcm", dtcm, sizeof(dtcm), DTCM_INIT_USED),
          "DTCM profile accepted");
    check(gnw_alloc_free_bytes(&pool) == 8216u,
          "DTCM effective free is 8,216 bytes, not nominal 81 KB");
    check(gnw_alloc_malloc(&pool, RC_REQUEST) == NULL,
          "rc 60 KB allocation is rejected before reaching hardware");

    check(gnw_alloc_pool_init(&pool, "ahb-old-gba", ahb, sizeof(ahb), AHB_OLD_GBA_STATIC),
          "pre-fix GBA AHB reservation accepted");
    check(gnw_alloc_free_bytes(&pool) == 79680u,
          "pre-fix AHB effective free is 79,680 bytes");
    check(gnw_alloc_malloc(&pool, DRAW2FB_REQUEST) == NULL,
          "32X 84 KB Draw2FB allocation reproduces the old overflow");

    check(gnw_alloc_pool_init(&pool, "poison", dtcm, 64u, 0),
          "poison pool initialized");
    uint8_t *plain = gnw_alloc_malloc(&pool, 12u);
    int poisoned = plain != NULL;
    for (int i = 0; poisoned && i < 12; i++) poisoned = plain[i] == 0xAA;
    check(poisoned, "malloc returns 0xAA poison, never accidental zeroes");
    uint8_t *cleared = gnw_alloc_calloc(&pool, 3u, 4u);
    int zeroed = cleared != NULL;
    for (int i = 0; zeroed && i < 12; i++) zeroed = cleared[i] == 0;
    check(zeroed, "calloc alone returns zero-filled bytes");
    check(gnw_alloc_calloc(&pool, (size_t)-1, 2u) == NULL,
          "calloc multiplication overflow is rejected");

    if (failures) printf("%d FAILURE(S)\n", failures);
    else printf("ALL PASS\n");
    return failures ? 1 : 0;
}
