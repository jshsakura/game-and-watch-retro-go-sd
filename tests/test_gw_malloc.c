/* Host unit test for Core/Src/gw_malloc.c: the bump allocators behind
 * itc_malloc/ahb_malloc/ram_malloc/ram_calloc (root CLAUDE.md's "RAM
 * priority = emulators first" rule is enforced here -- this file has no
 * test before this one).
 *
 * NOTE ON THE TASK-NAMED PATH: the task description named this file
 * "Core/Src/porting/gw_alloc.c", but every symbol it actually asked to pin
 * (itc_malloc/ahb_malloc/ram_malloc/ram_calloc, the "& ~0x03" alignment
 * rounding, the ITC bounds check against __ITCMRAM_LENGTH__ -
 * __NULLPTR_LENGTH__, an over-large request failing rather than wrapping)
 * lives in Core/Src/gw_malloc.c, not Core/Src/porting/gw_alloc.c (which is
 * just _sbrk() + a DEBUG_RG_ALLOC-gated malloc() wrapper -- no bump
 * pointers, no ITC/AHB pools, nothing the task's pins describe). Verified
 * with `grep -rn "void \*itc_malloc" .` -- Core/Src/gw_malloc.c is the only
 * definition in the tree. Testing the file that actually contains the
 * described logic.
 *
 * The linker symbols gw_malloc.c reads (__ITCMRAM_LENGTH__ etc.) are
 * address-as-value tricks (SIZEOF-as-address) -- given real values here via
 * -Wl,--defsym on the link line, same technique tests/test_clock_mp3.c
 * already uses for the overlay SIZE symbols. ram_start is a real writable
 * global (declared in gw_malloc.h, defined in gw_malloc.c), set directly. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include "gw_malloc.h"

/* Mirror gw_malloc.c's own extern decls so the test can compute expected
 * bounds from the same --defsym'd addresses. */
extern uint32_t __RAM_EMU_END__;
extern uint32_t __ahbram_heap_start__;
extern uint32_t __ahbram_audio_start__;
extern uint32_t __itcram_start__;
extern uint32_t __itcram_end__;
extern uint16_t __ITCMRAM_LENGTH__;
extern uint16_t __NULLPTR_LENGTH__;

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define ALIGNED4(p) (((uint32_t)(uintptr_t)(p) & 0x3u) == 0)

/* ---- ram_malloc: 4-byte alignment, from a deliberately misaligned
 * ram_start, across a run of odd-sized allocations. -------------------- */
static void test_ram_malloc_alignment(void) {
    ram_start = 0x20001; /* misaligned on purpose -- pins the (ram_start+3)&~3 rounding */
    for (int i = 0; i < 20; i++) {
        void *p = ram_malloc(1 + (i % 3)); /* sizes 1,2,3 repeating: never naturally aligned */
        CHECK(p != NULL, "ram_malloc unexpectedly returned NULL on iteration %d", i);
        CHECK(ALIGNED4(p), "ram_malloc returned misaligned pointer %p on iteration %d", p, i);
    }
    printf("  ram_malloc: 20 odd-sized allocs from misaligned ram_start, all 4-byte aligned\n");
}

/* ---- ram_malloc: an over-large request must fail (NULL), not wrap past
 * __RAM_EMU_END__ and hand back a pointer into whatever comes after it. -- */
static void test_ram_malloc_oob(void) {
    ram_start = 0x20000;
    size_t free_size = ram_get_free_size();
    void *ok = ram_malloc(free_size); /* exactly the whole pool: must succeed */
    CHECK(ok != NULL, "ram_malloc(exact free size) unexpectedly failed");
    CHECK(ram_get_free_size() == 0, "pool should read fully consumed after exact-size alloc (got %zu free)", ram_get_free_size());

    void *oob = ram_malloc(1); /* one more byte: must fail, not wrap */
    CHECK(oob == NULL, "ram_malloc past __RAM_EMU_END__ should return NULL, got %p", oob);

    ram_start = 0x20000;
    void *big = ram_malloc(free_size * 4); /* wildly over budget in one shot */
    CHECK(big == NULL, "ram_malloc(4x pool size) should fail outright, got %p", big);
    printf("  ram_malloc: over-large request fails rather than wrapping past __RAM_EMU_END__\n");
}

/* ---- itc_malloc: 4-byte alignment across odd sizes, and the bounds check
 * against __itcram_start__ + __ITCMRAM_LENGTH__ - __NULLPTR_LENGTH__. ---- */
static void test_itc_malloc(void) {
    uint32_t bound = (uint32_t)&__itcram_start__ + (uint32_t)&__ITCMRAM_LENGTH__ - (uint32_t)&__NULLPTR_LENGTH__;
    uint32_t start = (uint32_t)&__itcram_end__;
    uint32_t avail = bound - start;
    printf("  itc pool: [0x%x, 0x%x) = %u bytes available\n", start, bound, avail);

    itc_init();
    for (int i = 0; i < 5; i++) {
        void *p = itc_malloc(1 + (i % 3));
        CHECK(p != (void *)0xffffffff, "itc_malloc unexpectedly failed on small iteration %d", i);
        CHECK(ALIGNED4(p), "itc_malloc returned misaligned pointer %p on iteration %d", p, i);
    }

    /* Exact-fit then one-more-byte: consume the rest of the pool exactly,
     * then confirm the next byte is rejected with the sentinel, not wrapped
     * into whatever memory follows the pool. */
    itc_init();
    void *exact = itc_malloc(avail);
    CHECK(exact != (void *)0xffffffff, "itc_malloc(exact avail=%u) unexpectedly failed", avail);
    void *over = itc_malloc(1);
    CHECK(over == (void *)0xffffffff, "itc_malloc 1 byte past the ITC bound should fail with the sentinel, got %p", over);

    printf("  itc_malloc: alignment held across odd sizes; exact-fit ok, over-by-1 rejected\n");
}

/* ---- ahb_only_malloc: an over-large request hits the assert() bound check
 * (a hard failure) rather than silently wrapping past
 * __ahbram_audio_start__ into the DMA audio buffer region. Run in a child
 * process: assert() aborts, which is exactly "fails loudly" -- the parent
 * just confirms it did, rather than the process actually dying under gcov
 * mid-run. ------------------------------------------------------------- */
static void test_ahb_malloc_oob_asserts(void) {
    ahb_init();
    uint32_t budget = (uint32_t)&__ahbram_audio_start__ - (uint32_t)&__ahbram_heap_start__;

    pid_t pid = fork();
    if (pid == 0) {
        ahb_init();
        (void)ahb_only_malloc(budget * 4); /* wildly over budget */
        _exit(0); /* only reached if the assert did NOT fire -- test fails that below */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "ahb_only_malloc(4x budget) should abort() (assert), got status=0x%x", status);
    printf("  ahb_only_malloc: over-budget request asserts rather than wrapping into the audio region\n");
}

int main(void) {
    printf("=== gw_malloc.c: itc/ahb/ram bump allocators ===\n");
    test_ram_malloc_alignment();
    test_ram_malloc_oob();
    test_itc_malloc();
    test_ahb_malloc_oob_asserts();

    if (failures) {
        printf("%d assertion(s) FAILED\n", failures);
        return 1;
    }
    printf("all gw_malloc.c assertions passed\n");
    return 0;
}
