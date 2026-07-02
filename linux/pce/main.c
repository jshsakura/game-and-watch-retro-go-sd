#include <odroid_system.h>
             
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "porting.h"
#include "crc32.h"

#include <gfx.h>
#include "gw_lcd.h"
#include <pce.h>
#include <romdb.h>
#include "pce_cd.h"
#include "pce_scsi.h"
#include "pce_adpcm.h"

/* Path to the disc .cue (argv[1]); NULL = run System Card only. */
static const char *g_cue_path = NULL;

/* Deterministic per-instruction PC trace (host only). h6280_run() calls
 * pce_scsi_pc_tick() each instruction when g_pcecd_trace is set; we turn it on
 * right after the force-CLI so we capture EXACTLY what the game/vblank does
 * once interrupts are live — no Heisenbug (host timing is cycle-deterministic). */
static void dump_mem(uint16_t start, int len);   /* fwd */
int g_pcecd_trace = 0;
void pce_scsi_pc_tick(uint16_t pc)
{
    static int n = 0;          /* distinct (run-length-compressed) entries written */
    static int run = 0;        /* consecutive repeats of last_pc */
    static uint16_t last_pc = 0xFFFF;
    static FILE *tf = NULL;
    static int started = 0;
    static int dumped_entry = 0, dumped_idle = 0;
    /* Self-arm: ignore everything (incl. the long System Card boot) until the CPU
     * first lands on the REAL game exec entry (from the disc IPL: load=$6000,
     * exec=$6000), then trace the full game init from there. */
    #define GAME_EXEC 0x6000
    if (!started) { if (pc != GAME_EXEC) return; started = 1; }

    /* Ring buffer of the last 256 PCs. When the game FIRST reaches its hang loop
     * ($6257 = JMP self), dump the ring so we see EXACTLY the path (and the
     * conditional branch) that diverted into the halt — the decisive evidence. */
    #define RING 256
    static uint16_t ring[RING];
    static int ridx = 0, ring_done = 0;
    ring[ridx++ % RING] = pc;
    if (!ring_done && pc == 0x6257 && ridx > 1) {
        ring_done = 1;
        printf("[host] ===== PATH INTO $6257 HANG (last %d PCs) =====\n", RING);
        int start = ridx % RING;
        for (int k = 0; k < RING; k++) {
            int i = (start + k) % RING;
            printf("%04x ", ring[i]);
            if ((k % 16) == 15) printf("\n");
        }
        printf("\n[host] ===== END PATH =====\n");
        fflush(stdout);
    }
    /* Print the FIRST 16 instructions with the EXACT opcode the CPU fetches
     * (PageR[pc>>13][pc], same expression as imm_operand) vs pce_read8, plus the
     * bank mapping for that page — to settle the trace-vs-dump contradiction. */
    if (dumped_entry < 80) {
        dumped_entry++;
        extern uint8_t *PageR[8];
        uint8_t opc_raw = PageR[pc >> 13][pc];     /* what the CPU executes */
        uint8_t opc_r8  = pce_read8(pc);
        printf("[host] #%02d pc=%04x op(raw)=%02x op(r8)=%02x MMR[%d]=%02x A=%02x X=%02x Y=%02x S=%02x P=%02x\n",
               dumped_entry, pc, opc_raw, opc_r8, pc >> 13, PCE.MMR[pc >> 13],
               CPU_PCE.A, CPU_PCE.X, CPU_PCE.Y, CPU_PCE.S, CPU_PCE.P);
        fflush(stdout);
    }
    (void)dumped_idle;

    /* Game-space-only ring (pc < $E000 = not System Card ROM): records the last
     * GREC game instructions with regs/flags, dumped when the hang ($6257) is
     * first reached — so we see the game's OWN decision logic + the branch that
     * chose the dead-end, with all the BIOS noise filtered out. */
    #define GREC 200
    static struct { uint16_t pc, o; uint8_t a, x, y, p; } gr[GREC];
    static int gi = 0, gdone = 0;
    if (pc < 0xE000) {
        extern uint8_t *PageR[8];
        int s = gi % GREC;
        gr[s].pc = pc; gr[s].o = PageR[pc >> 13][pc];
        gr[s].a = CPU_PCE.A; gr[s].x = CPU_PCE.X; gr[s].y = CPU_PCE.Y; gr[s].p = CPU_PCE.P;
        gi++;
    }
    /* Targeted trace of the decisive BIOS call JSR $E0DE @ $6050 — whose returned
     * Carry decides hang ($6219) vs continue ($6058). Arm when we first reach the
     * $6050 call site; log every instruction (incl. System Card) until it returns
     * to $6053, so we see what $E0DE checks and where Carry gets set. */
    static int e0de = 0, e0de_arm = 0;
    if (!e0de_arm && pc == 0x6050) e0de_arm = 1;
    if (e0de_arm && e0de < 300) {
        e0de++;
        extern uint8_t *PageR[8];
        uint8_t o0 = PageR[pc >> 13][pc];
        printf("[host] E %04x:%02x A=%02x X=%02x Y=%02x P=%02x [C%d]\n",
               pc, o0, CPU_PCE.A, CPU_PCE.X, CPU_PCE.Y, CPU_PCE.P, CPU_PCE.P & 1);
        if (pc == 0x6053) e0de = 999;   /* stop after return */
        fflush(stdout);
    }

    if (!gdone && gi == 30000) {
        gdone = 1;
        printf("[host] ===== STEADY-STATE: last %d GAME INSTRS @ gi=30000 =====\n", GREC);
        int n = gi < GREC ? gi : GREC;
        int start = gi < GREC ? 0 : (gi % GREC);
        for (int k = 0; k < n; k++) {
            int i = (start + k) % GREC;
            printf("%04x:%02x[A%02x X%02x Y%02x P%02x] ", gr[i].pc, gr[i].o,
                   gr[i].a, gr[i].x, gr[i].y, gr[i].p);
            if ((k % 6) == 5) printf("\n");
        }
        printf("\n[host] ===== END GAME RING =====\n");
        fflush(stdout);
    }
    if (n >= 200000) return;
    if (!tf) tf = fopen("pc_trace.txt", "w");
    if (!tf) return;
    /* Run-length compress: idle loops (e.g. 6257 6257 ...) collapse to one line
     * "6257 xN" so the actual control flow / init sequence is readable. */
    if (pc == last_pc) { run++; return; }
    if (run > 0) { fprintf(tf, "x%d\n", run + 1); run = 0; }
    last_pc = pc;
    fprintf(tf, "%04x ", pc);
    n++;
    if (n == 200000) { fprintf(tf, "...cap\n"); fflush(tf); }
}

#undef printf
#define APP_ID 20

#define JOY_A       0x01
#define JOY_B       0x02
#define JOY_SELECT  0x04
#define JOY_RUN     0x08
#define JOY_UP      0x10
#define JOY_RIGHT   0x20
#define JOY_DOWN    0x40
#define JOY_LEFT    0x80

#define NVS_KEY_SAVE_SRAM "sram"

#define WIDTH    352
#define HEIGHT   242
#define BPP      2
#define SCALE    4

typedef uint16_t pixel_t;
static uint16_t mypalette[256];
#define COLOR_RGB(r, g, b) ((((r) << 13) & 0xf800) + (((g) << 8) & 0x07e0) + (((b) << 3) & 0x001f))


#define AUDIO_SAMPLE_RATE   (48000)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60)

// Use 60Hz for GB
#define AUDIO_BUFFER_LENGTH_GB (AUDIO_SAMPLE_RATE / 60)
#define AUDIO_BUFFER_LENGTH_DMA_GB ((2 * AUDIO_SAMPLE_RATE) / 60)

#define FB_INTERNAL_OFFSET (((XBUF_HEIGHT - current_height) / 2 + 16) * XBUF_WIDTH + (XBUF_WIDTH - current_width) / 2)
static uint8_t emulator_framebuffer_pce[XBUF_WIDTH * XBUF_HEIGHT * 2];

extern const unsigned char ROM_DATA[];
extern unsigned int cart_rom_len;


static odroid_video_frame_t update1 = {WIDTH, HEIGHT, WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};
static odroid_video_frame_t update2 = {WIDTH, HEIGHT, WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};

static bool saveSRAM = false;

// 3 pages
uint8_t state_save_buffer[192 * 1024];

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *fb_texture;
uint16_t fb_data[WIDTH * HEIGHT * BPP];

SDL_AudioSpec wanted;
void fill_audio(void *udata, Uint8 *stream, int len);

extern unsigned char cart_rom[];
extern unsigned int cart_rom_len;

static uint8_t PCE_EXRAM_BUF[0x8000];
static int framePerSecond=0;

static int current_height, current_width;
#define PCE_SAMPLE_RATE   (22050)
#define AUDIO_BUFFER_LENGTH_PCE  (PCE_SAMPLE_RATE / 60)
//static short audioBuffer_pce[ AUDIO_BUFFER_LENGTH_PCE * 2];

//The frames per second cap timer
int capTimer;

/**
 * Describes what is saved in a save state. Changing the order will break
 * previous saves so add a place holder if necessary. Eventually we could use
 * the keys to make order irrelevant...
 */
#define SVAR_1(k, v) { 1, k, &v }
#define SVAR_2(k, v) { 2, k, &v }
#define SVAR_4(k, v) { 4, k, &v }
#define SVAR_A(k, v) { sizeof(v), k, &v }
#define SVAR_N(k, v, n) { n, k, &v }
#define SVAR_END { 0, "\0\0\0\0", 0 }

static const char SAVESTATE_HEADER[8] = "PCE_V007";
static const struct
{
	size_t len;
	char key[16];
	void *ptr;
} SaveStateVars[] =
{
	// Arrays
	SVAR_A("RAM", PCE.RAM),      SVAR_A("VRAM", PCE.VRAM),  SVAR_A("SPRAM", PCE.SPRAM),
	SVAR_A("PAL", PCE.Palette),  SVAR_A("MMR", PCE.MMR),

	// CPU registers
	SVAR_2("CPU.PC", CPU_PCE.PC),    SVAR_1("CPU.A", CPU_PCE.A),    SVAR_1("CPU.X", CPU_PCE.X),
	SVAR_1("CPU.Y", CPU_PCE.Y),      SVAR_1("CPU.P", CPU_PCE.P),    SVAR_1("CPU.S", CPU_PCE.S),

	// Misc
	SVAR_4("Cycles", Cycles),                   SVAR_4("MaxCycles", PCE.MaxCycles),
	SVAR_1("SF2", PCE.SF2),                     SVAR_2("VBlankFL", PCE.VBlankFL),

	// IRQ
	SVAR_1("irq_mask", CPU_PCE.irq_mask),           SVAR_1("irq_mask_delay", CPU_PCE.irq_mask_delay),
	SVAR_1("irq_lines", CPU_PCE.irq_lines),

	// PSG
	SVAR_1("psg.ch", PCE.PSG.ch),               SVAR_1("psg.vol", PCE.PSG.volume),
	SVAR_1("psg.lfo_f", PCE.PSG.lfo_freq),      SVAR_1("psg.lfo_c", PCE.PSG.lfo_ctrl),
	SVAR_N("psg.ch0", PCE.PSG.chan[0], 40),     SVAR_N("psg.ch1", PCE.PSG.chan[1], 40),
	SVAR_N("psg.ch2", PCE.PSG.chan[2], 40),     SVAR_N("psg.ch3", PCE.PSG.chan[3], 40),
	SVAR_N("psg.ch4", PCE.PSG.chan[4], 40),     SVAR_N("psg.ch5", PCE.PSG.chan[5], 40),

	// VCE
    SVAR_1("vce_cr", PCE.VCE.CR),               SVAR_1("vce_dot_clock", PCE.VCE.dot_clock),
	SVAR_A("vce_regs", PCE.VCE.regs),           SVAR_2("vce_reg", PCE.VCE.reg),

	// VDC
	SVAR_A("vdc_regs", PCE.VDC.regs),           SVAR_1("vdc_reg", PCE.VDC.reg),
	SVAR_1("vdc_status", PCE.VDC.status),       SVAR_1("vdc_satb", PCE.VDC.vram),
	SVAR_1("vdc_satb", PCE.VDC.satb),			SVAR_4("vdc_pen_irqs", PCE.VDC.pending_irqs),

	// Timer
	SVAR_1("timer_reload", PCE.Timer.reload),   SVAR_1("timer_running", PCE.Timer.running),
	SVAR_1("timer_counter", PCE.Timer.counter), SVAR_4("timer_next", PCE.Timer.cycles_counter),
	SVAR_2("timer_freq", PCE.Timer.cycles_per_line),

	SVAR_END
};

void set_color(int index, uint8_t r, uint8_t g, uint8_t b) {
    uint16_t col = 0xffff;
    if (index != 255)  {
        col = COLOR_RGB(r,g,b);
    }
    mypalette[index] = col;
}

void init_color_pals() {
    printf("init_color_pals()\n");

    for (int i = 0; i < 255; i++) {
        // GGGRR RBB
          set_color(i, (i & 0x1C)>>2, (i & 0xE0) >> 5, (i & 0x03) );
    }
    set_color(255, 0x3f, 0x3f, 0x3f);
}

void odroid_display_force_refresh(void)
{
    // forceVideoRefresh = true;
}


void fill_audio(void *udata, Uint8 *stream, int len)
{

}

uint8_t *osd_gfx_framebuffer(void){
    return emulator_framebuffer_pce + FB_INTERNAL_OFFSET;
}

void osd_gfx_set_mode(int width, int height) {
	init_color_pals();
    printf("current_width: %d \ncurrent_height: %d\n", width, height);
    if (width < 160 || width > 512) {
		MESSAGE_ERROR("Correcting out of range screen w %d\n", width);
		width = 256;
	}
	if (height < 160 || height > 256) {
		MESSAGE_ERROR("Correcting out of range screen h %d\n", height);
		height = 224;
	}
    current_width = width;
    current_height = height;
    SDL_SetWindowSize( window, current_width * SCALE, current_height * SCALE);
    SDL_DestroyTexture(fb_texture);
    fb_texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
        current_width, current_height);

}

void pce_input_read(odroid_gamepad_state_t* out_state) {
    unsigned char rc = 0;
    if (out_state->values[ODROID_INPUT_LEFT])   rc |= JOY_LEFT;
    if (out_state->values[ODROID_INPUT_RIGHT])  rc |= JOY_RIGHT;
    if (out_state->values[ODROID_INPUT_UP])     rc |= JOY_UP;
    if (out_state->values[ODROID_INPUT_DOWN])   rc |= JOY_DOWN;
    if (out_state->values[ODROID_INPUT_A])      rc |= JOY_A;
    if (out_state->values[ODROID_INPUT_B])      rc |= JOY_B;
    if (out_state->values[ODROID_INPUT_START])  rc |= JOY_RUN;
    if (out_state->values[ODROID_INPUT_SELECT]) rc |= JOY_SELECT;
    PCE.Joypad.regs[0] = rc;
}

int init_window(int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return 0;

    window = SDL_CreateWindow("emulator",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        width * SCALE, height * SCALE,
        0);
    if (!window)
        return 0;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
        return 0;

    fb_texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
        width, height);
    if (!fb_texture)
        return 0;

    return 0;
}

static bool host_LoadState(const char *savePathName)
{
    printf("Loading state from %s...\n", savePathName);

	char buffer[512];

	FILE *fp = fopen(savePathName, "rb");
	if (fp == NULL)
		return -1;

	fread(&buffer, 8, 1, fp);

	if (memcmp(&buffer, SAVESTATE_HEADER, 8) != 0)
	{
		MESSAGE_ERROR("Loading state failed: Header mismatch\n");
		fclose(fp);
		return -1;
	}

	for (int i = 0; SaveStateVars[i].len > 0; i++)
	{
		printf("Loading %s (%d)\n", SaveStateVars[i].key, SaveStateVars[i].len);
		fread(SaveStateVars[i].ptr, SaveStateVars[i].len, 1, fp);
	}

	/* PCE-CD: restore the 256KB CD RAM streamed after the core state. */
	if (g_cue_path) {
		for (int v = 0x68; v <= 0x87; v++)
			fread(PCE.MemoryMapW[v], 0x2000, 1, fp);
		pce_scsi_reset();   /* mirror the device LoadState: SCSI back to idle */
		uint32_t cdda[1 + PCE_SCSI_CDDA_STATE_WORDS];
		if (fread(cdda, sizeof(cdda), 1, fp) == 1 && cdda[0] == 0x41444443u)
			pce_scsi_cdda_set(cdda + 1);
		/* ADPCM engine + 64KB RAM block (mirrors device LoadState). */
		uint32_t adpc[1 + PCE_ADPCM_STATE_WORDS];
		if (fread(adpc, sizeof(adpc), 1, fp) == 1 && adpc[0] == 0x43504441u) {
			fread(pce_adpcm_ram(), 1, 0x10000, fp);
			pce_adpcm_set(adpc + 1);
		} else {
			pce_adpcm_reset();
		}
	}

	for(int i = 0; i < 8; i++)
	{
		pce_bank_set(i, PCE.MMR[i]);
	}

	gfx_reset(true);

	osd_gfx_set_mode(IO_VDC_SCREEN_WIDTH, IO_VDC_SCREEN_HEIGHT);

	fclose(fp);

	/* Restore BRAM from its own file (the cabinet outlives any single .state). */
	if (g_cue_path) {
		FILE *bf = fopen("save_pce.bram", "rb");
		if (bf) { fread(PCE.bram, 1, 0x800, bf); fclose(bf); }
		pce_bram_format_if_needed();
	}

	return 0;
}

static bool host_SaveState(const char *savePathName)
{
    printf("Saving state to %s...\n", savePathName);

	FILE *fp = fopen(savePathName, "wb");
	if (fp == NULL)
		return -1;

	fwrite(SAVESTATE_HEADER, sizeof(SAVESTATE_HEADER), 1, fp);

	for (int i = 0; SaveStateVars[i].len > 0; i++)
	{
		printf("Saving %s (%d)\n", SaveStateVars[i].key, SaveStateVars[i].len);
		fwrite(SaveStateVars[i].ptr, SaveStateVars[i].len, 1, fp);
	}

	/* PCE-CD: stream the 256KB CD RAM (banks 0x68-0x87) after the core state,
	 * mirroring the device SaveState. */
	if (g_cue_path) {
		for (int v = 0x68; v <= 0x87; v++)
			fwrite(PCE.MemoryMapR[v], 0x2000, 1, fp);
		uint32_t cdda[1 + PCE_SCSI_CDDA_STATE_WORDS] = { 0x41444443u /* 'CDDA' */ };
		pce_scsi_cdda_get(cdda + 1);
		fwrite(cdda, sizeof(cdda), 1, fp);
		/* ADPCM engine + 64KB RAM block (mirrors device SaveState). */
		uint32_t adpc[1 + PCE_ADPCM_STATE_WORDS] = { 0x43504441u /* 'ADPC' */ };
		pce_adpcm_get(adpc + 1);
		fwrite(adpc, sizeof(adpc), 1, fp);
		fwrite(pce_adpcm_ram(), 1, 0x10000, fp);
	}

	fclose(fp);

	/* BRAM persisted to its own file, independent of the .state snapshot. */
	if (g_cue_path) {
		FILE *bf = fopen("save_pce.bram", "wb");
		if (bf) { fwrite(PCE.bram, 1, 0x800, bf); fclose(bf); }
	}

	return 0;
}

void pcm_submit(void)
{

}

size_t
pce_osd_getromdata(unsigned char **data)
{
    /* src pointer to the ROM data in the external flash (raw or LZ4) */
    *data = (unsigned char *)ROM_DATA;
    return cart_rom_len;
}

const struct {
	const uint32_t crc;
	const char *Name;
	const uint32_t Flags;
} pceRomFlags[] = {
	{0x00000000, "Unknown", JAP},
	{0xF0ED3094, "Blazing Lazers", USA | TWO_PART_ROM},
	{0xB4A1B0F6, "Blazing Lazers", USA | TWO_PART_ROM},
	{0x55E9630D, "Legend of Hero Tonma", USA | US_ENCODED},
	{0x083C956A, "Populous", JAP | ONBOARD_RAM},
	{0x0A9ADE99, "Populous", JAP | ONBOARD_RAM},
};

int LoadCard(const char *name) {
    int offset;
    size_t rom_length = pce_osd_getromdata(&PCE.ROM);
    offset = rom_length & 0x1fff;
       
       
       PCE.ROM_SIZE = (rom_length - offset) / 0x2000;
       PCE.ROM_DATA = PCE.ROM + offset;
       PCE.ROM_CRC = crc32_le(0, PCE.ROM, rom_length);
       
       uint8_t IDX = 0;
       uint8_t ROM_MASK = 1;

       while (ROM_MASK < PCE.ROM_SIZE) ROM_MASK <<= 1;
       ROM_MASK--;

       printf("Rom Size: %d, B1:%X, B2:%X, B3:%X, B4:%X\n" , rom_length, PCE.ROM[0], PCE.ROM[1],PCE.ROM[2],PCE.ROM[3]);

       for (int index = 0; index < KNOWN_ROM_COUNT; index++) {
           if (PCE.ROM_CRC == pceRomFlags[index].crc) {
               IDX = index;
               break;
           }
       }

       printf("Game Name: %s\n", pceRomFlags[IDX].Name);
       printf("Game Region: %s\n", (pceRomFlags[IDX].Flags & JAP) ? "Japan" : "USA");

       // US Encrypted
    if ((pceRomFlags[IDX].Flags & US_ENCODED) || PCE.ROM_DATA[0x1FFF] < 0xE0) {

		unsigned char inverted_nibble[16] = {
			0, 8, 4, 12, 2, 10, 6, 14,
			1, 9, 5, 13, 3, 11, 7, 15
		};

		for (int x = 0; x < PCE.ROM_SIZE * 0x2000; x++) {
			unsigned char temp = PCE.ROM_DATA[x] & 15;

			PCE.ROM_DATA[x] &= ~0x0F;
			PCE.ROM_DATA[x] |= inverted_nibble[PCE.ROM_DATA[x] >> 4];

			PCE.ROM_DATA[x] &= ~0xF0;
			PCE.ROM_DATA[x] |= inverted_nibble[temp] << 4;
		}
    }

	// For example with Devil Crush 512Ko
    if (pceRomFlags[IDX].Flags & TWO_PART_ROM) 
        PCE.ROM_SIZE = 0x30;

    // Game ROM
    for (int i = 0; i < 0x80; i++) {
        if (PCE.ROM_SIZE == 0x30) {
            switch (i & 0x70) {
            case 0x00:
            case 0x10:
            case 0x50:
                PCE.MemoryMapR[i] = PCE.ROM_DATA + (i & ROM_MASK) * 0x2000;
                break;
            case 0x20:
            case 0x60:
                PCE.MemoryMapR[i] = PCE.ROM_DATA + ((i - 0x20) & ROM_MASK) * 0x2000;
                break;
            case 0x30:
            case 0x70:
                PCE.MemoryMapR[i] = PCE.ROM_DATA + ((i - 0x10) & ROM_MASK) * 0x2000;
                break;
            case 0x40:
                PCE.MemoryMapR[i] = PCE.ROM_DATA + ((i - 0x20) & ROM_MASK) * 0x2000;
                break;
            }
        } else {
            PCE.MemoryMapR[i] = PCE.ROM_DATA + (i & ROM_MASK) * 0x2000;
        }
        PCE.MemoryMapW[i] = PCE.NULLRAM;
    }

    // Allocate the card's onboard ram
    if (pceRomFlags[IDX].Flags & ONBOARD_RAM) {
        PCE.ExRAM = PCE.ExRAM ?: PCE_EXRAM_BUF;
        PCE.MemoryMapR[0x40] = PCE.MemoryMapW[0x40] = PCE.ExRAM;
        PCE.MemoryMapR[0x41] = PCE.MemoryMapW[0x41] = PCE.ExRAM + 0x2000;
        PCE.MemoryMapR[0x42] = PCE.MemoryMapW[0x42] = PCE.ExRAM + 0x4000;
        PCE.MemoryMapR[0x43] = PCE.MemoryMapW[0x43] = PCE.ExRAM + 0x6000;
    }

    // Mapper for roms >= 1.5MB (SF2, homebrews)
    if (PCE.ROM_SIZE >= 192)
        PCE.MemoryMapW[0x00] = PCE.IOAREA;

    return 0;
}

int
InitPCE(int samplerate, bool stereo, const char *huecard)
{
	if (gfx_init())
		return 1;

//	if (psg_init(samplerate, stereo))
//		return 1;

	if (pce_init())
		return 1;

	if (huecard && LoadCard(huecard))
		return 1;

	/* PCE-CD host harness: map 256KB CD RAM (banks 0x68-0x87) like the device
	 * LoadCartPCE does, then mount the real CUE/BIN so the System Card's $1800
	 * SCSI reads hit it. pce_cd_read_sector uses fopen/fread = works on host. */
	if (g_cue_path) {
		static uint8_t *cdram = NULL;
		if (!cdram) cdram = (uint8_t *)malloc(0x40000);
		memset(cdram, 0, 0x40000);
		for (int v = 0x68; v <= 0x87; v++)
			PCE.MemoryMapR[v] = PCE.MemoryMapW[v] = cdram + (uint32_t)(v - 0x68) * 0x2000;
		static pce_cd_toc_t s_toc;
		if (pce_cd_parse_cue(g_cue_path, &s_toc)) {
			pce_scsi_set_disc(&s_toc, true);
			printf("CD mounted: %s (%d tracks)\n", g_cue_path, s_toc.num_tracks);
		} else {
			pce_scsi_set_disc(NULL, false);
			printf("CD mount FAILED: %s\n", g_cue_path);
		}
		/* BRAM: map bank $F7, load save_pce.bram, format-init if absent. */
		pce_bram_init();
		FILE *bf = fopen("save_pce.bram", "rb");
		if (bf) { fread(PCE.bram, 1, 0x800, bf); fclose(bf); }
		pce_bram_format_if_needed();
	}

	gfx_reset(0);
	pce_reset(0);

	return 0;
}

void init(void)
{
    printf("init()\n");
    odroid_system_init(APP_ID, AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&host_LoadState, &host_SaveState, NULL, NULL, NULL, NULL);

    // Hack: Use the same buffer twice
    update1.buffer = fb_data;
    update2.buffer = fb_data;

    //saveSRAM = odroid_settings_app_int32_get(NVS_KEY_SAVE_SRAM, 0);
    saveSRAM = false;

    // Load ROM
    InitPCE(0,0,"game.pce");

    // Video
    memset(fb_data, 0, sizeof(fb_data));
}

void pce_osd_gfx_blit(bool drawFrame) {
    static uint32_t lastFPSTime = 0;
    static uint32_t frames = 0;
    static int wantedTime = 1000 / 60;
    int xScale = 0;
    int y=0, offsetY, offsetX = 0;
    uint8_t *fbTmp;

    if (!drawFrame) {
        memset(fb_data,0,sizeof(fb_data));
        return;
    }

    uint32_t currentTime = HAL_GetTick();
    uint32_t delta = currentTime - lastFPSTime;


    odroid_display_scaling_t scaling = ODROID_DISPLAY_SCALING_OFF;
    
    if (current_width > 0 && scaling != ODROID_DISPLAY_SCALING_OFF) {
        xScale =  (current_width << 8) / WIDTH ;
    } else offsetX = (WIDTH - current_width)/2; //center the image horizontally

    offsetX = 0;

    int renderHeight = (current_height<=HEIGHT)?current_height:HEIGHT;

    uint8_t *emuFrameBuffer = osd_gfx_framebuffer();
    pixel_t *framebuffer_active = fb_data;//lcd_get_active_buffer();

    if (delta >= 1000) {
        framePerSecond = (10000 * frames) / delta;
        printf("FPS: %d.%d, frames %d, delta %d ms\n", framePerSecond / 10, framePerSecond % 10, frames, delta);
        frames = 0;
        lastFPSTime = currentTime;
    }

    for(y=0;y<renderHeight;y++) {
        fbTmp = emuFrameBuffer+(y*XBUF_WIDTH);
        offsetY = y*current_width;
        if (xScale) {
            // Horizontal - Scale 
            for(int x=0;x<WIDTH;x++) {
                framebuffer_active[offsetY+x]= mypalette[fbTmp[ (x * xScale) >> 8 ]];
            }
        } else {
            // No scaling, 1:1
            for(int x=0;x<current_width;x++) {
                   framebuffer_active[offsetY+x+offsetX]=mypalette[fbTmp[x]];
            }
        }
    }
    //* Temporary, Y scaling is not yet implemented
    /*for(;y<HEIGHT;y++) {
        fbTmp = emuFrameBuffer+(y*XBUF_WIDTH);
        offsetY = y*WIDTH;
        for(int x=0;x<WIDTH;x++) {
            framebuffer_active[offsetY+x+offsetX]=0;
        }
    }*/

    SDL_UpdateTexture(fb_texture, NULL, fb_data, current_width * BPP);
    SDL_RenderCopy(renderer, fb_texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    memset(fb_data,0,sizeof(fb_data));

    //If frame finished early
    int frameTicks = SDL_GetTicks() - capTimer;
    if( frameTicks < wantedTime )
    {
        //Wait remaining time
        SDL_Delay( wantedTime - frameTicks );
    }
}


void odroid_input_read_gamepad_pce(odroid_gamepad_state_t* out_state)
{
    SDL_Event event;
    static SDL_Event last_down_event;

    if (SDL_PollEvent(&event)) {
        if (event.type == SDL_KEYDOWN) {
            // printf("Press %d\n", event.key.keysym.sym);
            switch (event.key.keysym.sym) {
            case SDLK_x:
                out_state->values[ODROID_INPUT_A] = 1;
                break;
            case SDLK_z:
                out_state->values[ODROID_INPUT_B] = 1;
                break;
            case SDLK_LSHIFT:
                out_state->values[ODROID_INPUT_START] = 1;
                break;
            case SDLK_LCTRL:
                out_state->values[ODROID_INPUT_SELECT] = 1;
                break;
            case SDLK_UP:
                out_state->values[ODROID_INPUT_UP] = 1;
                break;
            case SDLK_DOWN:
                out_state->values[ODROID_INPUT_DOWN] = 1;
                break;
            case SDLK_LEFT:
                out_state->values[ODROID_INPUT_LEFT] = 1;
                break;
            case SDLK_RIGHT:
                out_state->values[ODROID_INPUT_RIGHT] = 1;
                break;
            case SDLK_ESCAPE:
                exit(1);
                break;
            default:
                break;
            }
            last_down_event = event;
        } else if (event.type == SDL_KEYUP) {
            // printf("Release %d\n", event.key.keysym.sym);
            switch (event.key.keysym.sym) {
            case SDLK_x:
                out_state->values[ODROID_INPUT_A] = 0;
                break;
            case SDLK_z:
                out_state->values[ODROID_INPUT_B] = 0;
                break;
            case SDLK_LSHIFT:
                out_state->values[ODROID_INPUT_START] = 0;
                break;
            case SDLK_LCTRL:
                out_state->values[ODROID_INPUT_SELECT] = 0;
                break;
            case SDLK_UP:
                out_state->values[ODROID_INPUT_UP] = 0;
                break;
            case SDLK_DOWN:
                out_state->values[ODROID_INPUT_DOWN] = 0;
                break;
            case SDLK_LEFT:
                out_state->values[ODROID_INPUT_LEFT] = 0;
                break;
            case SDLK_RIGHT:
                out_state->values[ODROID_INPUT_RIGHT] = 0;
                break;
            case SDLK_F1:
                if (last_down_event.key.keysym.sym == SDLK_F1)
                    host_SaveState("save_pce.bin");
                break;
            case SDLK_F4:
                if (last_down_event.key.keysym.sym == SDLK_F4)
                    host_LoadState("save_pce.bin");
                break;                
            default:
                break;
            }
        }
    }
}

void osd_log(int type, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
}

/* One-shot dump of the CPU bank mapping (MMR) + the System Card RAM hook-vector
 * area ($2200-$225F lives in PCE.RAM offset 0x200). Lets us SEE whether the game
 * registered a vsync/IRQ handler pointing at its own code (0x6xxx/0x8xxx). */
static void dump_mem(uint16_t start, int len)
{
    for (int a = start; a < start + len; a += 16) {
        printf("[host] $%04x:", a);
        for (int j = 0; j < 16; j++) printf(" %02x", pce_read8((uint16_t)(a + j)));
        printf("\n");
    }
}

static void dump_state(const char *tag)
{
    printf("[host] === STATE %s ===\n", tag);
    printf("[host] MMR:");
    for (int i = 0; i < 8; i++) printf(" %d:%02x", i, PCE.MMR[i]);
    printf("\n");
    for (int base = 0x200; base < 0x260; base += 16) {
        printf("[host] $%04x:", 0x2000 + base);
        for (int j = 0; j < 16; j++) printf(" %02x", PCE.RAM[base + j]);
        printf("\n");
    }
    printf("[host] --- exec vector $2280-$228F ---\n"); dump_mem(0x2280, 16);
    printf("[host] --- program @ $6250-$62BF ---\n");   dump_mem(0x6250, 112);
    printf("[host] --- subroutine @ $6350-$637F ---\n"); dump_mem(0x6350, 48);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    if (argc > 1) g_cue_path = argv[1];
    /* Headless trace mode: argv[2] = number of frames to run then exit (so the
     * deterministic SCSI/PC diag can be inspected without an infinite loop). */
    int max_frames = (argc > 2) ? atoi(argv[2]) : 0;

    init_window(WIDTH, HEIGHT);

    init();
    odroid_gamepad_state_t joystick = {0};
    int frame = 0;
    bool forced_cli = false;
    bool dumped_6257 = false;
    /* argv[3] == "cli" → force-CLI at 0x6257 (run the game IRQ-enabled). Without
     * it we observe the game's NATURAL init: does it CLI / register a vsync hook
     * on its own, or idle at 0x6257 with interrupts still masked? */
    bool do_cli = (argc > 3 && strcmp(argv[3], "cli") == 0);
    g_pcecd_trace = 1;   /* tracer self-arms at first 0x6254 (game entry) */

    while (true)
    {

        //Start cap timer
        capTimer = SDL_GetTicks();
        //wdog_refresh();
        bool drawFrame = true;// common_emu_frame_loop();

        odroid_input_read_gamepad_pce(&joystick);
        /* Headless: auto-press START (RUN) over frames 60-120 to boot the disc
         * from the "CD-ROM SYSTEM" screen (no human to press it). */
        joystick.values[ODROID_INPUT_START] = ((frame % 200) >= 60 && (frame % 200) < 90) ? 1 : 0;
        pce_input_read(&joystick);

        /* Same per-frame hook the device main loop calls: pumps the chunked
         * SCSI->ADPCM DMA (<=8KB/frame) so ADPCM loads complete. */
        pce_scsi_run();

        /* First time the game reaches its idle loop, dump the registered hook
         * vectors + bank mapping so we can see what (if anything) init set up. */
        if (!dumped_6257 && CPU_PCE.PC == 0x6257) {
            dumped_6257 = true;
            printf("[host] reached 0x6257 idle frame=%d P=%02x irqmask=%02x\n",
                   frame, CPU_PCE.P, CPU_PCE.irq_mask);
            dump_state("at-6257");
        }

        /* Optional: same interrupt-enable the device harness uses — the System
         * Card hands off with FL_I set; clear it once so VBlank IRQs run. */
        if (do_cli && !forced_cli && CPU_PCE.PC == 0x6257 && (CPU_PCE.P & 0x04)) {
            forced_cli = true;
            CPU_PCE.P &= ~0x04;
            printf("[host] FORCE-CLI at 0x6257 frame=%d\n", frame);
        }

        for (PCE.Scanline = 0; PCE.Scanline < 263; ++PCE.Scanline) {
            gfx_run();
        }
        pce_osd_gfx_blit(drawFrame);
        //if(drawFrame) pce_pcm_submit();

        /* CD-DA verify: dump this frame's CD-DA PCM (44100 stereo s16,
         * bit-perfect passthrough) to a raw file so we can confirm the audio
         * decode produces real music. */
        {
            static FILE *cf = NULL, *af = NULL;
            static int16_t cb[800 * 2], ab[800 * 2];
            if (!cf) cf = fopen("cdda.pcm", "wb");
            if (!af) af = fopen("adpcm.pcm", "wb");
            int n = pce_scsi_cdda_fill(cb, 735);
            if (cf && n > 0) fwrite(cb, sizeof(int16_t) * 2, n, cf);
            /* Resume regression probe: across the frame-1100/1110 save/load
             * self-test, n must stay >0 (the CD-DA block re-arms the stream). */
            if (g_cue_path && frame >= 1105 && frame <= 1115)
                printf("[host] f%d cdda_n=%d\n", (int)frame, n);
            extern int pce_adpcm_fill(int16_t *, int);
            int an = pce_adpcm_fill(ab, 735);
            if (af && an > 0) fwrite(ab, sizeof(int16_t) * 2, an, af);
        }

        // Prevent overflow
        PCE.Timer.cycles_counter -= Cycles;
        PCE.MaxCycles -= Cycles;
        Cycles = 0;

        /* Save/load round-trip self-test (PCE-CD): save at 1100, corrupt the CD
         * RAM at 1105, load at 1110 — the checksum must return to the saved one. */
        if (g_cue_path && max_frames >= 1115) {
            static uint32_t sum_pre = 0;
            if (frame == 1100) {
                for (int v = 0x68; v <= 0x87; v++)
                    for (int k = 0; k < 0x2000; k++) sum_pre += PCE.MemoryMapR[v][k];
                host_SaveState("save_pce.bin");
                printf("[host] SAVE @1100 cdram_sum=%08x\n", sum_pre);
            } else if (frame == 1105) {
                memset(PCE.MemoryMapW[0x80], 0xEE, 0x2000);   /* corrupt one bank */
                uint32_t s = 0;
                for (int v = 0x68; v <= 0x87; v++)
                    for (int k = 0; k < 0x2000; k++) s += PCE.MemoryMapR[v][k];
                printf("[host] CORRUPT @1105 cdram_sum=%08x (should differ)\n", s);
            } else if (frame == 1110) {
                host_LoadState("save_pce.bin");
                uint32_t s = 0;
                for (int v = 0x68; v <= 0x87; v++)
                    for (int k = 0; k < 0x2000; k++) s += PCE.MemoryMapR[v][k];
                printf("[host] LOAD @1110 cdram_sum=%08x -> %s\n", s,
                       s == sum_pre ? "MATCH (save/load OK)" : "MISMATCH (BUG)");
            }
        }

        if (max_frames && ++frame >= max_frames) {
            printf("[host] done %d frames, PC=%04x P=%02x irql=%02x\n",
                   frame, CPU_PCE.PC, CPU_PCE.P, CPU_PCE.irq_lines);
            dump_state("end");
            /* Dump the live framebuffer to a PPM so we can SEE what's on screen. */
            {
                FILE *pf = fopen("frame_end.ppm", "wb");
                if (pf) {
                    uint8_t *efb = osd_gfx_framebuffer();
                    fprintf(pf, "P6\n%d %d\n255\n", current_width, current_height);
                    for (int yy = 0; yy < current_height; yy++) {
                        uint8_t *rowp = efb + yy * XBUF_WIDTH;
                        for (int xx = 0; xx < current_width; xx++) {
                            uint16_t px = mypalette[rowp[xx]];
                            uint8_t r = ((px >> 11) & 0x1f) << 3;
                            uint8_t g = ((px >> 5) & 0x3f) << 2;
                            uint8_t b = (px & 0x1f) << 3;
                            fputc(r, pf); fputc(g, pf); fputc(b, pf);
                        }
                    }
                    fclose(pf);
                    printf("[host] wrote frame_end.ppm %dx%d\n", current_width, current_height);
                }
            }
            break;
        }
    }

    SDL_Quit();

    return 0;
}
