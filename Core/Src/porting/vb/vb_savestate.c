/*
 * Virtual Boy save/load state — device module.
 *
 * Reuses red-viper's proven savestate format (vb_gui.c emulation_sstate/lstate,
 * header "RVSS" = 0x53535652, version 2): the FULL machine state — CPU regs,
 * VIP regs, hardware-control regs, sound state, and all four RAM regions.
 *
 * Difference from vb_gui.c: this is path-based (takes the exact file path the
 * retro-go firmware passes to SaveState/LoadState) and drops the 3DS GUI deps
 * and the pre-v2 backward-compat blocks (we only ever write v2). fopen works on
 * device via FatFs and on the host harness via stdio, so the SAME code is
 * round-trip verified on the host before flashing.
 *
 * Returns 0 on success, non-zero on failure (matching vb_gui.c convention).
 */

#include <stdio.h>
#include <stdint.h>

#include "v810_cpu.h"
#include "v810_mem.h"
#include "vb_set.h"
#include "vb_sound.h"

#define VB_SS_ID   0x53535652u  /* "RVSS" */
#define VB_SS_VER  2u

int vb_state_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return 1;
    }

    uint32_t size;

    #define FWRITE(buffer, sz, count, stream) \
        if (fwrite(buffer, sz, count, stream) < (size_t)(count)) goto bail;
    #define WRITE_VAR(V) FWRITE(&(V), 1, sizeof(V), f)

    /* Header */
    uint32_t id  = VB_SS_ID;
    uint32_t ver = VB_SS_VER;
    uint32_t crc = (uint32_t)tVBOpt.CRC32;
    WRITE_VAR(id);
    WRITE_VAR(ver);
    WRITE_VAR(crc);

    /* CPU registers */
    WRITE_VAR(vb_state->v810_state.P_REG);
    WRITE_VAR(vb_state->v810_state.S_REG);
    WRITE_VAR(vb_state->v810_state.PC);
    WRITE_VAR(vb_state->v810_state.cycles);
    WRITE_VAR(vb_state->v810_state.except_flags);

    /* VIP registers */
    size = (uint32_t)sizeof(vb_state->tVIPREG);
    WRITE_VAR(size);
    WRITE_VAR(vb_state->tVIPREG);

    /* Hardware control registers */
    size = (uint32_t)sizeof(vb_state->tHReg);
    WRITE_VAR(size);
    WRITE_VAR(vb_state->tHReg);

    /* Audio registers */
    size = (uint32_t)sizeof(sound_state);
    WRITE_VAR(size);
    WRITE_VAR(sound_state);

    /* RAM regions */
    #define WRITE_MEMORY(area) \
        size = (uint32_t)(vb_state->area.highaddr + 1 - vb_state->area.lowaddr); \
        WRITE_VAR(size); \
        FWRITE(vb_state->area.pmemory, 1, size, f);
    WRITE_MEMORY(V810_DISPLAY_RAM);
    WRITE_MEMORY(V810_SOUND_RAM);
    WRITE_MEMORY(V810_VB_RAM);
    WRITE_MEMORY(V810_GAME_RAM);
    #undef WRITE_MEMORY
    #undef WRITE_VAR
    #undef FWRITE

    fclose(f);
    return 0;

bail:
    fclose(f);
    return 1;
}

int vb_state_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 1;
    }

    #define FREAD(buffer, sz, count, stream) \
        if (fread(buffer, sz, count, stream) < (size_t)(count)) goto bail;

    /* Header */
    uint32_t id = 0, ver = 0, crc = 0;
    FREAD(&id, 4, 1, f);
    FREAD(&ver, 4, 1, f);
    if (id != VB_SS_ID || ver != VB_SS_VER) {
        goto bail;
    }
    FREAD(&crc, 4, 1, f);
    if (crc != (uint32_t)tVBOpt.CRC32) {
        goto bail;  /* savestate is for a different ROM */
    }

    /* Read into a scratch copy first; only commit if the whole read succeeds. */
    cpu_state new_state = vb_state->v810_state;
    uint32_t size;

    #define READ_VAR(V) FREAD(&(V), 1, sizeof(V), f)
    READ_VAR(new_state.P_REG);
    READ_VAR(new_state.S_REG);
    READ_VAR(new_state.PC);
    READ_VAR(new_state.cycles);
    READ_VAR(new_state.except_flags);

    /* VIP registers — size must match this build's struct (v2 only). */
    READ_VAR(size);
    if (size != (uint32_t)sizeof(vb_state->tVIPREG)) goto bail;
    READ_VAR(vb_state->tVIPREG);

    /* Hardware control registers */
    READ_VAR(size);
    if (size != (uint32_t)sizeof(vb_state->tHReg)) goto bail;
    READ_VAR(vb_state->tHReg);

    /* Audio registers */
    READ_VAR(size);
    if (size != (uint32_t)sizeof(sound_state)) goto bail;
    READ_VAR(sound_state);

    /* RAM regions */
    #define READ_MEMORY(area) \
        READ_VAR(size); \
        if (size != (uint32_t)(vb_state->area.highaddr + 1 - vb_state->area.lowaddr)) goto bail; \
        FREAD(vb_state->area.pmemory, 1, size, f);
    READ_MEMORY(V810_DISPLAY_RAM);
    READ_MEMORY(V810_SOUND_RAM);
    READ_MEMORY(V810_VB_RAM);
    READ_MEMORY(V810_GAME_RAM);
    #undef READ_MEMORY
    #undef READ_VAR
    #undef FREAD

    /* Commit CPU state now that the full payload read cleanly. */
    vb_state->v810_state = new_state;

    fclose(f);
    return 0;

bail:
    fclose(f);
    return 1;
}
