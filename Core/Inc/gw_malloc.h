#ifndef _GW_MALLOC_H_
#define _GW_MALLOC_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

extern uint32_t ram_start;

/* Core-specific AHB pool base (phase-0 gate 3). Cores are exclusive
 * overlays and every core exit reboots through the launcher's ahb_init(),
 * which restores the global pool base (__ahbram_heap_start__, above the GBA
 * statics). A core whose AHB footprint is core-exclusive may rebase its own
 * session's pool lower -- e.g. Sega CD reusing the GBA BIOS/cheats/sound
 * statics at 0x30000000 that only exist while GBA is resident -- and must
 * re-claim after any internal ahb_init() re-run. Statelessly rebases the
 * bump pointer; the ceiling (__ahbram_audio_start__, the audio DMA reserve)
 * never moves. */
void ahb_set_core_base(uint32_t base);

void ahb_init();
void *ahb_malloc(size_t size);
void *ahb_only_malloc(size_t size);
void *ahb_calloc(size_t count,size_t size);
/* Headroom left in the AHB dynamic pool before ahb_only_malloc's bound
 * assert (__ahbram_audio_start__) fires. ahb_only_malloc does NOT return
 * NULL on overflow like ram_malloc does -- it advances the bump pointer
 * past the bound and hits that assert, so callers that need to fail soft
 * must check headroom BEFORE allocating, not after. */
size_t ahb_get_free_size();

void itc_init();
void *itc_malloc(size_t size);
void *itc_calloc(size_t count,size_t size);

size_t ram_get_free_size();
/* mark/release: snapshot the bump pointer and roll it back — lets a launcher
 * app (e.g. the Clock GIF background) borrow the big emu-RAM pool temporarily
 * without starving later cover/list allocations. Only valid LIFO. */
size_t ram_mark(void);
void ram_release(size_t mark);
void *ram_malloc(size_t size);
void *ram_calloc(size_t count,size_t size);

/* DTCM stdlib heap (_heap_start.._heap_end). Use for emulator overlays
 * (PICO-8 p8ram, PCE work RAM, etc.) that need free/realloc. */
void *dtcm_malloc(size_t size);
void *dtcm_calloc(size_t count, size_t size);
void dtcm_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
