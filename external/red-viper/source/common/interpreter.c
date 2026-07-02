#include <math.h>
#include "v810_cpu.h"
#include "v810_mem.h"
#include "v810_opt.h"
#include "vb_types.h"
#include "drc_core.h"

/* Instruction-fetch fast path. mem_rhword() is a call + address-space switch on
 * EVERY guest instruction (~290k/frame; 27% of host runtime, worse on the M7).
 * The PC is almost always in ROM (mirrored at 0x07 with a power-of-2 mask) or
 * WRAM (0x05); fetch those inline and fall back to mem_rhword for anything else. */
extern unsigned int vb_rom_mask;   /* v810_mem.c */
static inline HWORD fetch_hword(WORD PC) {
    if (likely((PC & 0x07000000) == 0x07000000))
        return *(HWORD *)(V810_ROM1.off + (PC & (0x07000000 | vb_rom_mask) & ~1));
    if ((PC >> 24) == 0x05)
        return *(HWORD *)(vb_state->V810_VB_RAM.off + (PC & 0x0500fffe));
    return (HWORD)mem_rhword(PC);
}

/* Data-access fast paths — the fetch_hword trick applied to the LD/ST hot cases.
 * WRAM (0x05) dominates data traffic; ROM (0x07) covers table reads. CRITICAL:
 * the idle-skip poll signature is normally set inside mem_r*, so the inline WRAM
 * read must keep that bookkeeping (poll loops read WRAM mailboxes!); ROM reads
 * are excluded from the signature by design. Everything else (VIP/hw/VSU/SRAM)
 * falls back to the full dispatchers for their side effects. */
extern bool vb_idle_wrote, vb_idle_hwread;
extern WORD vb_idle_raddr;
static inline WORD data_rbyte(WORD addr) {
    if ((addr >> 24) == 0x05) { vb_idle_hwread = true; vb_idle_raddr = addr;
        return *(BYTE *)(vb_state->V810_VB_RAM.off + (addr & 0x0500ffff)); }
    if ((addr & 0x07000000) == 0x07000000)
        return *(BYTE *)(V810_ROM1.off + (addr & (0x07000000 | vb_rom_mask)));
    return (WORD)mem_rbyte(addr);
}
static inline WORD data_rhword(WORD addr) {
    if ((addr >> 24) == 0x05) { vb_idle_hwread = true; vb_idle_raddr = addr;
        return *(HWORD *)(vb_state->V810_VB_RAM.off + (addr & 0x0500fffe)); }
    if ((addr & 0x07000000) == 0x07000000)
        return *(HWORD *)(V810_ROM1.off + (addr & (0x07000000 | vb_rom_mask) & ~1));
    return (WORD)mem_rhword(addr);
}
static inline WORD data_rword(WORD addr) {
    if ((addr >> 24) == 0x05) { vb_idle_hwread = true; vb_idle_raddr = addr;
        return *(WORD *)(vb_state->V810_VB_RAM.off + (addr & 0x0500fffc)); }
    if ((addr & 0x07000000) == 0x07000000)
        return *(WORD *)(V810_ROM1.off + (addr & (0x07000000 | vb_rom_mask) & ~3));
    return (WORD)mem_rword(addr);
}
static inline void data_wbyte(WORD addr, WORD v) {
    if ((addr >> 24) == 0x05) { vb_idle_wrote = true;
        *(BYTE *)(vb_state->V810_VB_RAM.off + (addr & 0x0500ffff)) = v; return; }
    mem_wbyte(addr, v);
}
static inline void data_whword(WORD addr, WORD v) {
    if ((addr >> 24) == 0x05) { vb_idle_wrote = true;
        *(HWORD *)(vb_state->V810_VB_RAM.off + (addr & 0x0500fffe)) = v; return; }
    mem_whword(addr, v);
}
static inline void data_wword(WORD addr, WORD v) {
    if ((addr >> 24) == 0x05) { vb_idle_wrote = true;
        *(WORD *)(vb_state->V810_VB_RAM.off + (addr & 0x0500fffc)) = v; return; }
    mem_wword(addr, v);
}

static bool get_cond(BYTE code, WORD psw) {
    bool cond = false;
    switch (0x40 | (code & ~8)) {
        case V810_OP_BV: cond = psw & 4; break;
        case V810_OP_BL: cond = psw & 8; break;
        case V810_OP_BE: cond = psw & 1; break;
        case V810_OP_BNH: cond = psw & 9; break;
        case V810_OP_BN: cond = psw & 2; break;
        case V810_OP_BR: cond = true; break;
        case V810_OP_BLT: cond = !!(psw & 4) != !!(psw & 2); break;
        case V810_OP_BLE: cond = (psw & 1) || !!(psw & 4) != !!(psw & 2); break;
    }
    if (code & 8) cond = !cond;
    return cond;
}

int interpreter_run(void) {
    /* Hoist the cpu_state pointer: vb_state is a GLOBAL pointer, so without this
     * the compiler must reload it (and re-derive every register address) after
     * every external call — thousands of extra loads per frame on the M7. It
     * cannot change mid-run on device (single player). */
    cpu_state * const CPU = &vb_state->v810_state;
    // keep PC and cycles in local variables for extra speed
    // can't do this with PSW because interrupts modify it
    WORD PC = CPU->PC;
    WORD last_PC = PC;
    WORD cycles = CPU->cycles;
    BYTE last_opcode = 0;
    WORD target = cycles;
    do {
        if ((SWORD)(target - cycles) <= 0) {
            CPU->PC = PC;
            if (serviceInt(cycles, PC) && (PC != CPU->PC || CPU->ret)) {
                // interrupt triggered, so we exit
                // PC may have been modified so don't reset it
                CPU->cycles = cycles;
                return 0;
            }
            target = cycles + CPU->cycles_until_event_partial;
        }
        HWORD instr = fetch_hword(PC);
        PC += 2;
        BYTE opcode = instr >> 10;
        BYTE reg1 = instr & 31;
        BYTE reg2 = (instr >> 5) & 31;
        cycles += opcycle[opcode];
        if (opcode < 0x20) {
            // small instr
            WORD reg1_val = 0;
            if (!(opcode & 0x10) && reg1) reg1_val = CPU->P_REG[reg1];
            switch (opcode) {
                case V810_OP_MOV:
                    CPU->P_REG[reg2] = reg1_val;
                    break;
                case V810_OP_ADD: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    WORD res = reg2_val + reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg2_val ^ reg1_val) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg2_val;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SUB: case V810_OP_CMP: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    WORD res = reg2_val - reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)((reg2_val ^ reg1_val) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)reg2_val < (unsigned)reg1_val;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    if (opcode == V810_OP_SUB) CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SHL: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = reg2_val << reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (32 - reg1_val)) & 1 : 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SHR: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = reg2_val >> reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (reg1_val - 1)) & 1 : 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_JMP:
                    PC = reg1_val;
                    break;
                case V810_OP_SAR: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = (SWORD)reg2_val >> reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (reg1_val - 1)) & 1 : 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_MUL: {
                    SWORD reg2_val = reg2 ? (SWORD)CPU->P_REG[reg2] : 0;
                    int64_t res = (int64_t)(SWORD)reg1_val * (int64_t)reg2_val;
                    bool ov = res != (int64_t)(int32_t)res;
                    bool z = res == 0;
                    bool s = res < 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2);
                    CPU->P_REG[30] = (WORD)(res >> 32);
                    CPU->P_REG[reg2] = (WORD)res;
                    break;
                }
                case V810_OP_DIV: {
                    SWORD reg2_val = reg2 ? (SWORD)CPU->P_REG[reg2] : 0;
                    if (reg2_val == 0x80000000 && (SWORD)reg1_val == -1) {
                        CPU->P_REG[30] = 0;
                        CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | 6;
                    } else {
                        CPU->P_REG[30] = reg2_val % (SWORD)reg1_val;
                        SWORD res = reg2_val / (SWORD)reg1_val;
                        bool z = res == 0;
                        bool s = res < 0;
                        CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | z | (s << 1);
                        CPU->P_REG[reg2] = res;
                    }
                    break;
                }
                case V810_OP_MULU: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    uint64_t res = (uint64_t)reg1_val * (uint64_t)reg2_val;
                    bool ov = res != (uint64_t)(uint32_t)res;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2);
                    CPU->P_REG[30] = (WORD)(res >> 32);
                    CPU->P_REG[reg2] = (WORD)res;
                    break;
                }
                case V810_OP_DIVU: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    CPU->P_REG[30] = reg2_val % reg1_val;
                    WORD res = reg2_val / reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | z | (s << 1);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_OR: {
                    WORD res = (reg2 ? CPU->P_REG[reg2] : 0) | reg1_val;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_AND: {
                    WORD res = (reg2 ? CPU->P_REG[reg2] : 0) & reg1_val;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_XOR: {
                    WORD res = (reg2 ? CPU->P_REG[reg2] : 0) ^ reg1_val;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_NOT: {
                    WORD res = ~reg1_val;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_MOV_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    CPU->P_REG[reg2] = imm;
                    break;
                }
                case V810_OP_ADD_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    WORD res = reg2_val + imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg2_val ^ imm) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg2_val;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SETF: {
                    CPU->P_REG[reg2] = get_cond(reg1, CPU->S_REG[PSW]);
                    break;
                }
                case V810_OP_CMP_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    WORD res = reg2_val - imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)((reg2_val ^ imm) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)reg2_val < (unsigned)imm;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    if (opcode == V810_OP_SUB) CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SHL_I: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    WORD res = reg2_val << reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (32 - reg1)) & 1 : 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SHR_I: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    WORD res = reg2_val >> reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (reg1 - 1)) & 1 : 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_CLI:
                    CPU->S_REG[PSW] &= ~(1 << 12);
                    break;
                case V810_OP_SAR_I: {
                    WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                    WORD res = (SWORD)reg2_val >> reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (reg1 - 1)) & 1 : 0;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                // case V810_OP_TRAP:
                case V810_OP_RETI:
                    if (CPU->S_REG[PSW] & PSW_NP) {
                        PC = CPU->S_REG[FEPC];
                        CPU->S_REG[PSW] = CPU->S_REG[FEPSW];
                    } else {
                        PC = CPU->S_REG[EIPC];
                        CPU->S_REG[PSW] = CPU->S_REG[EIPSW];
                    }
                    break;
                case V810_OP_HALT: {
                    cycles = target;
                    CPU->PC = PC;
                    do {
                        cycles += CPU->cycles_until_event_partial;
                        CPU->cycles_until_event_partial = CPU->cycles_until_event_full = 0;
                        CPU->cycles = cycles;
                        serviceInt(cycles, PC);
                    } while (!CPU->ret && CPU->PC == PC);
                    if (CPU->PC == PC) {
                        // no interrupt triggered, so repeat the halt
                        CPU->PC = last_PC;
                    }
                    // PC was modified so don't reset it
                    return 0;
                }
                case V810_OP_LDSR:
                    CPU->S_REG[reg1] = (reg2 ? CPU->P_REG[reg2] : 0);
                    break;
                case V810_OP_STSR:
                    CPU->P_REG[reg2] = CPU->S_REG[reg1];
                    break;
                case V810_OP_SEI:
                    CPU->S_REG[PSW] |= 1 << 12;
                    break;
                case V810_OP_BSTR: {
                    typedef bool (*bstr_func)(WORD,WORD,WORD,WORD);
                    bstr_func func = (bstr_func)bssuboptable[reg1].func;
                    WORD lastarg = reg1 < 4 ? CPU->P_REG[27] & 31 : ((CPU->P_REG[27] & 31)) | ((CPU->P_REG[26] & 31) << 5) | ((target - cycles) << 10);
                    WORD res = func(CPU->P_REG[30], CPU->P_REG[29], CPU->P_REG[28], lastarg);
                    if (reg1 < 4) {
                        CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~1) | !res;
                    } else {
                        CPU->cycles += res;
                        if (CPU->P_REG[28]) {
                            PC = last_PC;
                        }
                    }
                    break;
                }
                default: {
                    CPU->PC = last_PC;
                    return DRC_ERR_BAD_INST;
                }
            }
        } else if (opcode < 0x28) {
            // branch
            if (get_cond(instr >> 9, CPU->S_REG[PSW])) {
                SHWORD disp = instr & (1 << 8) ? (instr | 0xfe00) : (instr & ~0xfe00);
                PC += disp - 2;
                /* Idle-loop skip: games spend most of a frame in tiny poll loops
                 * (read a VIP/HW status reg, test, branch back). Emulating every
                 * spin burns the whole 20ms budget on the M7. A taken SHORT
                 * BACKWARD branch to the same target, whose body read a hw/VIP
                 * status register and wrote NOTHING, is such a poll: fast-forward
                 * to the next event (`target`), where serviceInt can change the
                 * polled state. Register-only delay loops never set hwread and
                 * are left untouched; any store disqualifies via vb_idle_wrote. */
                if (disp < 0 && disp > -64) {
                    extern bool vb_idle_wrote, vb_idle_hwread;
                    extern WORD vb_idle_raddr;
                    extern unsigned int vb_stat_skips;
                    static WORD s_idle_pc, s_idle_raddr;
                    static int  s_idle_spins;
                    /* Same loop AND same polled address each spin: scan/checksum
                     * loops (advancing reads, register-only) must not skip — they
                     * would inflate emulated time and slow the GAME to a crawl. */
                    if (PC == s_idle_pc && vb_idle_raddr == s_idle_raddr) {
                        if (vb_idle_hwread && !vb_idle_wrote && ++s_idle_spins >= 3) {
                            if ((SWORD)(target - cycles) > 0) { cycles = target; vb_stat_skips++; }
                            s_idle_spins = 0;
                        }
                    } else {
                        s_idle_pc = PC;
                        s_idle_raddr = vb_idle_raddr;
                        s_idle_spins = 0;
                    }
                    vb_idle_wrote = false;
                    vb_idle_hwread = false;
                }
            } else {
                // branch not taken, so it only took 1 cycle
                cycles -= 2;
            }
        } else {
            // long instr
            HWORD instr2 = fetch_hword(PC);
            PC += 2;
            switch (opcode) {
                case V810_OP_MOVEA: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    CPU->P_REG[reg2] = reg1_val + (SHWORD)instr2;
                    break;
                }
                case V810_OP_ADDI: {
                    WORD reg1_val = reg1 ? CPU->P_REG[reg1] : 0;
                    WORD imm = (SHWORD)instr2;
                    WORD res = reg1_val + imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg1_val ^ imm) & (reg1_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg1_val;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_JAL:
                    CPU->P_REG[31] = PC;
                    // fallthrough
                case V810_OP_JR: {
                    SWORD disp = instr2 | ((SWORD)instr << 16);
                    if (disp & 0x02000000) disp |= 0xfc000000;
                    else disp &= ~(0xfc000000);
                    PC += disp - 4;
                    break;
                }
                case V810_OP_ORI: {
                    WORD res = instr2;
                    if (reg1) res |= CPU->P_REG[reg1];
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_ANDI: {
                    WORD res = 0;
                    if (reg1) res = CPU->P_REG[reg1] & instr2;
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_XORI: {
                    WORD res = instr2;
                    if (reg1) res ^= CPU->P_REG[reg1];
                    CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    CPU->P_REG[reg2] = res;
                    break;
                }
                case V810_OP_MOVHI: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    CPU->P_REG[reg2] = reg1_val + ((WORD)instr2 << 16);
                    break;
                }
                case V810_OP_LD_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    CPU->P_REG[reg2] = (SBYTE)data_rbyte(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 2 cycles instead of 3
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_LD_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    CPU->P_REG[reg2] = (SHWORD)data_rhword(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 2 cycles instead of 3
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_LD_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    CPU->P_REG[reg2] = data_rword(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 4 cycles instead of 5
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 4;
                    }
                    break;
                }
                case V810_OP_IN_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    CPU->P_REG[reg2] = (BYTE)data_rbyte(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 2 cycles instead of 3
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_IN_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    CPU->P_REG[reg2] = (HWORD)data_rhword(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 2 cycles instead of 3
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_IN_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    CPU->P_REG[reg2] = (WORD)data_rword(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 4 cycles instead of 5
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 4;
                    }
                    break;
                }
                case V810_OP_ST_B: case V810_OP_OUT_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    BYTE reg2_val = 0;
                    if (reg2) reg2_val = CPU->P_REG[reg2];
                    data_wbyte(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        // with two consecutive stores, the second takes 2 cycles instead of 1
                        cycles += 1;
                    }
                    break;
                }
                case V810_OP_ST_H: case V810_OP_OUT_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    HWORD reg2_val = 0;
                    if (reg2) reg2_val = CPU->P_REG[reg2];
                    data_whword(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        // with two consecutive stores, the second takes 2 cycles instead of 1
                        cycles += 1;
                    }
                    break;
                }
                case V810_OP_ST_W: case V810_OP_OUT_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = CPU->P_REG[reg1];
                    WORD reg2_val = 0;
                    if (reg2) reg2_val = CPU->P_REG[reg2];
                    data_wword(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        // with two consecutive stores, the second takes 4 cycles instead of 1
                        cycles += 3;
                    }
                    break;
                }
                // case V810_OP_CAXI:
                case V810_OP_FPP: {
                    int subop = instr2 >> 10;
                    #pragma GCC diagnostic push
                    #pragma GCC diagnostic ignored "-Wstrict-aliasing"
                    if (subop == V810_OP_CVT_WS) {
                        float res = reg1 ? (float)(SWORD)CPU->P_REG[reg1] : 0;
                        bool z = res == 0;
                        int scy = res < 0 ? 0xa : 0;
                        CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | scy;
                        *(float*)&CPU->P_REG[reg2] = res;
                    } else if (!(subop & 8) || subop == V810_OP_TRNC_SW) {
                        // float
                        float reg1_val = reg1 ? *(float*)&CPU->P_REG[reg1] : 0;
                        if (subop == V810_OP_CVT_SW) {
                            SWORD res = round(reg1_val);
                            bool z = res == 0;
                            int scy = res < 0 ? 2 : 0;
                            CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | scy;
                            CPU->P_REG[reg2] = res;
                        } else if (subop == V810_OP_TRNC_SW) {
                            SWORD res = (SWORD)(reg1_val);
                            bool z = res == 0;
                            int scy = res < 0 ? 2 : 0;
                            CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | scy;
                            CPU->P_REG[reg2] = res;
                        } else {
                            float reg2_val = reg2 ? *(float*)&CPU->P_REG[reg2] : 0;
                            float res;
                            switch (subop) {
                                case V810_OP_ADDF_S:
                                    res = reg2_val + reg1_val;
                                    break;
                                case V810_OP_CMPF_S:
                                case V810_OP_SUBF_S:
                                    res = reg2_val - reg1_val;
                                    break;
                                case V810_OP_MULF_S:
                                    res = reg2_val * reg1_val;
                                    break;
                                case V810_OP_DIVF_S:
                                    res = reg2_val / reg1_val;
                                    break;
                                default:
                                    return DRC_ERR_BAD_INST;
                            }
                            bool z = res == 0;
                            int scy = res < 0 ? 0xa : 0;
                            CPU->S_REG[PSW] = (CPU->S_REG[PSW] & ~0xf) | z | scy;
                            if (subop != V810_OP_CMPF_S) *(float*)&CPU->P_REG[reg2] = res;
                        }
                    } else {
                        // extended
                        WORD reg2_val = reg2 ? CPU->P_REG[reg2] : 0;
                        switch (subop) {
                            case V810_OP_MPYHW:
                                CPU->P_REG[reg2] *= reg1 ? (int)(CPU->P_REG[reg1] << 15) >> 15 : 0;
                                break;
                            case V810_OP_REV:
                                CPU->P_REG[reg2] = reg1 ? ins_rev(CPU->P_REG[reg1]) : 0;
                                break;
                            case V810_OP_XB:
                                CPU->P_REG[reg2] = (reg2_val & 0xFFFF0000) | ((reg2_val << 8) & 0xFF00) | ((reg2_val >> 8) & 0xFF);
                                break;
                            case V810_OP_XH:
                                CPU->P_REG[reg2] = (reg2_val << 16) | (reg2_val >> 16);
                                break;
                            default:
                                return DRC_ERR_BAD_INST;
                        }
                    }
                    #pragma GCC diagnostic pop
                    break;
                }
                default: {
                    CPU->PC = last_PC;
                    return DRC_ERR_BAD_INST;
                }
            }
        }
        last_opcode = opcode;
        if ((PC & 0x07000000) < 0x05000000) {
            CPU->PC = last_PC;
            return DRC_ERR_BAD_PC;
        }
        last_PC = PC;
    } while (!CPU->ret && (!DRC_AVAILABLE || (PC & 0x07000000) != 0x07000000));
    CPU->PC = PC;
    CPU->cycles = cycles;
    return 0;
}
