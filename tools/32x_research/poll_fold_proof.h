/* Isolated rig proof. Calls the original and replacement full fast-loop
 * helpers with the same context, code, SDRAM and rejection-cache contents. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned poll_proof_seed = 0x81723591u;
static unsigned poll_proof_random(void)
{
    poll_proof_seed ^= poll_proof_seed << 13;
    poll_proof_seed ^= poll_proof_seed >> 17;
    return poll_proof_seed ^= poll_proof_seed << 5;
}

void poll_fold_proof(void)
{
    static unsigned ram[0x40000 / 4];
    unsigned region = gnw_fw_rom.region;
    unsigned saved_reject[2][GNW_DL_REJ_SLOTS];
    memcpy(saved_reject, gnw_dl_reject, sizeof(saved_reject));
    gnw_fw_rom.region = 0x02000000;
    unsigned cases = 0;
    for (unsigned i = 0; i < 65536; i++) {
        SH2 initial, a, b;
        memset(&initial, 0, sizeof(initial));
        for (unsigned r = 0; r < 16; r++) initial.r[r] = poll_proof_random();
        initial.p_sdram = ram;
        initial.is_slave = i & 1;
        initial.ppc = 0x06000204;
        initial.pc = initial.ppc + 2;
        initial.ea = poll_proof_random();
        initial.sr = poll_proof_random() & 0x3f3;
        initial.gbr = poll_proof_random();
        initial.vbr = poll_proof_random();
        initial.pr = poll_proof_random();
        initial.mach = poll_proof_random();
        initial.macl = poll_proof_random();
        initial.t_flag = (i >> 1) & 1;
        /* Every remainder, zero/negative budgets and large complete loops. */
        initial.icount = (int)(i % 2048) - 5;
        unsigned dest = (i >> 2) & 15;
        unsigned base = (dest + 1) & 15;
        unsigned mask = (dest + 2) & 15;
        unsigned ref = (dest + 3) & 15;
        unsigned raw = poll_proof_random();
        unsigned m = poll_proof_random();
        if ((i & 31) == 0) m = 0;
        if ((i & 31) == 4) m = ~0u;
        initial.r[base] = 0x06001000 | ((i & 8) ? 0x20000000 : 0);
        initial.r[mask] = m;
        initial.r[ref] = (i & 16) ? raw & m : poll_proof_random();
        initial.r[dest] = (i & 32) ? raw : initial.r[ref];
        /* Register alias guards, unaligned polled address, malformed code,
         * non-SDRAM polled address and forward branches decline identically. */
        unsigned variant = (i >> 10) & 15;
        if (variant == 1) base = dest;
        if (variant == 2) mask = dest;
        if (variant == 3) ref = dest;
        if (variant == 4) initial.r[base] |= 1;
        if (variant == 5) initial.r[base] = 0x20004000;
        if (variant == 6) initial.r[base] = 0x07001000;
        if (variant == 7) initial.r[base] = 0x0603fffc;
        unsigned n = (i & 64) ? ref : dest;
        unsigned cm = (i & 64) ? dest : ref;
        unsigned short *code = (unsigned short *)((unsigned char *)ram + 0x200);
        code[0] = 0x3000 | (n << 8) | (cm << 4);
        code[1] = 0x6002 | (dest << 8) | (base << 4);
        code[2] = 0x8dfc;
        code[3] = 0x2009 | (dest << 8) | (mask << 4);
        if (variant == 8) code[0] = 0x0009;
        if (variant == 9) code[1] ^= 1;
        if (variant == 10) code[3] ^= 1;
        ram[(initial.r[base] & 0x3fffc) / 4] = CPU_BE2(raw);
        unsigned opcode = variant == 11 ? 0x8d01 : 0x8dfc;
        if (variant == 12) initial.ppc = 0x00000204; /* rejected fetch region */
        a = initial;
        b = initial;
        unsigned before[2][GNW_DL_REJ_SLOTS], expected[2][GNW_DL_REJ_SLOTS];
        for (unsigned c = 0; c < 2; c++)
            for (unsigned s = 0; s < GNW_DL_REJ_SLOTS; s++)
                before[c][s] = poll_proof_random();
        memcpy(gnw_dl_reject, before, sizeof(before));
        poll_reference_fastloop(&a, opcode);
        memcpy(expected, gnw_dl_reject, sizeof(expected));
        memcpy(gnw_dl_reject, before, sizeof(before));
        gnw_sh2_fastloop(&b, opcode);
        if (memcmp(&a, &b, sizeof(a)) ||
            memcmp(expected, gnw_dl_reject, sizeof(expected))) {
            printf("POLL_FOLD_PROOF_FAIL case=%u variant=%u budget=%d a=%d b=%d\n",
                   i, variant, initial.icount, a.icount, b.icount);
            exit(2);
        }
        cases++;
    }
    gnw_fw_rom.region = region;
    memcpy(gnw_dl_reject, saved_reject, sizeof(saved_reject));
    printf("POLL_FOLD_PROOF_PASS cases=%u context_bytes=%u reject_cache_equal=1\n",
           cases, (unsigned)sizeof(SH2));
}
