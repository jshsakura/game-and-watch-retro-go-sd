// license:BSD-3-Clause
// copyright-holders:Wilbert Pol, Robbbert
//
// Freestanding C port of the Tiger Game.com machine (MAME gamecom_m.cpp /
// gamecom_v.cpp / gamecom.cpp).  Bus, banking, blitter-DMA, timers and a
// software scanline renderer + SG0/SG1 wavetable  software scanline renderer.  Audio is stored but not yet synthesised. DAC audio.

#include "gamecom_core.h"
#include "sm8500.h"
#include <string.h>

#ifndef BIT
#define BIT(x,n) (((x) >> (n)) & 1)
#endif

/* --- SM8521 register addresses (subset used) --- */
enum {
	SM8521_IE0 = 0x10, SM8521_IE1 = 0x11, SM8521_IR0 = 0x12, SM8521_IR1 = 0x13,
	SM8521_P0 = 0x14, SM8521_P1 = 0x15, SM8521_P2 = 0x16, SM8521_P3 = 0x17,
	SM8521_SYS = 0x19, SM8521_PS0 = 0x1E, SM8521_PS1 = 0x1F,
	SM8521_MMU0 = 0x24, SM8521_MMU1 = 0x25, SM8521_MMU2 = 0x26,
	SM8521_MMU3 = 0x27, SM8521_MMU4 = 0x28,
	SM8521_LCDC = 0x30, SM8521_LCH = 0x31, SM8521_LCV = 0x32,
	SM8521_DMC = 0x34, SM8521_DMX1 = 0x35, SM8521_DMY1 = 0x36,
	SM8521_DMDX = 0x37, SM8521_DMDY = 0x38, SM8521_DMX2 = 0x39,
	SM8521_DMY2 = 0x3A, SM8521_DMPL = 0x3B, SM8521_DMBR = 0x3C, SM8521_DMVP = 0x3D,
	SM8521_SGC = 0x40, SM8521_SG0L = 0x42, SM8521_SG1L = 0x44,
	SM8521_SG0TH = 0x46, SM8521_SG0TL = 0x47, SM8521_SG1TH = 0x48,
	SM8521_SG1TL = 0x49, SM8521_SG2L = 0x4A, SM8521_SG2TH = 0x4C,
	SM8521_SG2TL = 0x4D, SM8521_SGDA = 0x4E,
	SM8521_TM0C = 0x50, SM8521_TM0D = 0x51, SM8521_TM1C = 0x52,
	SM8521_TM1D = 0x53, SM8521_CLKT = 0x54,
	SM8521_SG0W0 = 0x60, SM8521_SG0W15 = 0x6F,
	SM8521_SG1W0 = 0x70, SM8521_SG1W15 = 0x7F
};

/* CPU clock = XTAL(11.0592MHz)/2; refresh ~59.73Hz */
#define GC_CLOCK   (11059200 / 2)
#define GC_FPS_NUM 59732155     /* 59.732155 Hz scaled */
#define CYCLES_PER_FRAME ((int)(((int64_t)GC_CLOCK * 1000000) / GC_FPS_NUM))
#define Y_PIXELS 200

/* --- memory ---
 * Small buffers live in the core's BSS (overlay on device, ~30KB total).
 * The 256KB kernel BIOS and the up-to-2MB cartridge are NOT copied to RAM: they
 * are referenced in place (flash XIP on device, caller buffer on PC) by
 * pointer+size, and bank/DMA windows are mapped analytically (no 2MB mirror). */
static uint8_t  ram[0x400];          /* 0x0000-0x03FF work RAM + register file */
static uint8_t  videoram[0x4000];    /* 0xA000-0xDFFF */
static uint8_t  nvram[0x2000];       /* 0xE000-0xFFFF */
static uint8_t  gc_zero_page[0x4000];/* read-as-0 fill for unmapped bank windows */

static const uint8_t *internal_rom;  /* 0x1000-0x1FFF internal BIOS (>=4KB) */
static const uint8_t *kernel_rom;    /* external "kernel" PDA firmware (<=256KB) */
static uint32_t       kernel_size;
static const uint8_t *cart_base;     /* cartridge image, in place (<=2MB) */
static uint32_t       cart_size;
static int            cart_present;

static const uint8_t *bank1, *bank2, *bank3, *bank4;
static const uint8_t *cart_ptr;      /* set via P3; NULL or cart_base */
static uint8_t       *m_p_ram = ram; /* MAME alias */

/* Map a kernel-BIOS byte offset to an in-place pointer (256KB, power of two). */
static const uint8_t *kernel_window(uint32_t off)
{
	if (!kernel_rom || !kernel_size) return gc_zero_page;
	return kernel_rom + (off & (kernel_size - 1));
}

/* Map a cartridge byte offset (0..2MB space) to an in-place pointer, replicating
 * MAME's load/mirror layout without a physical 2MB copy. Bank/DMA windows are
 * >=8KB-aligned and never straddle a region boundary. */
static const uint8_t *cart_window(uint32_t off)
{
	if (!cart_present) return gc_zero_page;
	if (cart_size == 0x1c0000)   /* MAME: loaded at +0x40000 in a zeroed 2MB image */
		return (off >= 0x40000 && off < 0x200000) ? cart_base + (off - 0x40000) : gc_zero_page;
	if ((cart_size & (cart_size - 1)) == 0)         /* power of two: mirror by masking */
		return cart_base + (off & (cart_size - 1));
	return (off < cart_size) ? cart_base + off : gc_zero_page;  /* odd size: clamp */
}

/* --- input mux state (active low; 0xFF = nothing pressed) --- */
static uint8_t io_in0 = 0xFF, io_in1 = 0xFF, io_in2 = 0xFF;
/* touchscreen: 13 columns (X) x 10 rows (Y), one-hot per column when touched */
#define GC_GRID_COLS 13
#define GC_GRID_ROWS 10
static uint16_t io_grid[GC_GRID_COLS];   /* 10-bit one-hot row mask per column */

/* --- video --- */
uint8_t gamecom_fb[GAMECOM_W * GAMECOM_H];
uint8_t gamecom_palette[5][3];
static uint16_t m_scanline;

/* --- DMA (blitter) --- */
typedef struct {
	uint8_t width_x, width_y, source_x, source_x_current, source_y, source_width;
	uint8_t dest_x, dest_x_current, dest_y, dest_width, palette, block_width, block_height;
	const uint8_t *source_bank; uint16_t source_current, source_line, source_mask;
	uint8_t       *dest_bank;   uint16_t dest_current, dest_line, dest_mask;
	uint8_t transfer_mode; int16_t adjust_x; int decrement_y, overwrite_mode;
} GAMECOM_DMA;
static GAMECOM_DMA m_dma;

/* --- hardware timers TM0/TM1 --- */
typedef struct { int enabled; uint32_t prescale_count, prescale_max; uint8_t upcounter_max; } GAMECOM_TIMER;
static GAMECOM_TIMER m_timer[2];
static const int gamecom_timer_limit[8] = { 2, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };

/* --- real-time clock (CLKT) — coarse frame-based approximation --- */
static int clock_enabled, clock_minutes;
static int clock_frame_acc;

/* --- sound registers (synthesised in gamecom_audio_mix) --- */
typedef struct {
	uint8_t sgc, sg0l, sg1l, sg2l, sgda;
	uint16_t sg0t, sg1t, sg2t;
	uint8_t sg0w[16], sg1w[16];
} gamecom_sound_t;
static gamecom_sound_t m_sound;
/* wavetable phase accumulators (in 0..32 waveform-step units) */
static float sg0_phase, sg1_phase;
/* DAC capture: the game streams 8-bit samples by writing SGDA at the TIM1 rate
 * (MAME: "dac pitch is controlled by how often TIM1_INT occurs"). We record each
 * write within a frame and resample the lot across the frame's output samples. */
#define GC_DAC_FIFO 8192
static uint8_t  dac_fifo[GC_DAC_FIFO];
static uint16_t dac_count;

/* ============================ banking ============================ */
static void gamecom_set_mmu(uint8_t mmu, uint8_t data)
{
	const uint8_t *base;
	if (data < 0x20)
		base = kernel_window((uint32_t)data << 13);
	else if (cart_ptr)
		base = cart_window((uint32_t)data << 13);
	else
		return;
	switch (mmu) {
		case 1: bank1 = base; break;
		case 2: bank2 = base; break;
		case 3: bank3 = base; break;
		case 4: bank4 = base; break;
	}
}

/* ============================ input ============================ */
static void handle_stylus_press(int column)
{
	uint16_t data = (column >= 0 && column < GC_GRID_COLS) ? io_grid[column] : 0;
	if (data) {
		uint16_t stylus_y = data ^ 0x3ff;
		m_p_ram[SM8521_P0] = stylus_y & 0xff;
		m_p_ram[SM8521_P1] = (m_p_ram[SM8521_P1] & 0xFC) | (stylus_y >> 8);
	} else {
		m_p_ram[SM8521_P0] = 0xFF;
		m_p_ram[SM8521_P1] |= 3;
	}
}

/* Tap/release the touchscreen at screen pixel (x,y); pressed=0 lifts the stylus. */
void gamecom_set_stylus(int x, int y, int pressed)
{
	for (int c = 0; c < GC_GRID_COLS; c++) io_grid[c] = 0;
	if (!pressed) return;
	if (x < 0) x = 0;
	if (x >= GAMECOM_W) x = GAMECOM_W - 1;
	if (y < 0) y = 0;
	if (y >= GAMECOM_H) y = GAMECOM_H - 1;
	/* layout: 16px cells; cols 0..11 over x=0..191, col 12 over x=192..199; rows = y/16 */
	int col = x / 16; if (col > 12) col = 12;
	int row = y / 16; if (row > 9) row = 9;
	io_grid[col] = (uint16_t)(1u << row);
}

static void handle_input_press(uint16_t mux_data)
{
	switch (mux_data) {
		case 0xFFFB: handle_stylus_press(0);  break;
		case 0xFFF7: handle_stylus_press(1);  break;
		case 0xFFEF: handle_stylus_press(2);  break;
		case 0xFFDF: handle_stylus_press(3);  break;
		case 0xFFBF: handle_stylus_press(4);  break;
		case 0xFF7F: handle_stylus_press(5);  break;
		case 0xFEFF: handle_stylus_press(6);  break;
		case 0xFDFF: handle_stylus_press(7);  break;
		case 0xFBFF: handle_stylus_press(8);  break;
		case 0xF7FF: handle_stylus_press(9);  break;
		case 0xEFFF: handle_stylus_press(10); break;
		case 0xDFFF: handle_stylus_press(11); break;
		case 0xBFFF: handle_stylus_press(12); break;
		case 0x7FFF:    /* keys #1: dpad + menu/pause/sound + A,B,C */
			m_p_ram[SM8521_P0] = io_in0;
			m_p_ram[SM8521_P1] = (m_p_ram[SM8521_P1] & 0xFC) | (io_in1 & 3);
			break;
		case 0xFFFF:    /* keys #2: power + button D */
			m_p_ram[SM8521_P0] = (m_p_ram[SM8521_P0] & 0xFC) | (io_in2 & 3);
			m_p_ram[SM8521_P1] = 0xFF;
			break;
	}
}

void gamecom_set_input_state(uint8_t in0, uint8_t in1, uint8_t in2)
{
	io_in0 = in0; io_in1 = in1; io_in2 = in2;
}

/* ============================ I/O writes ============================ */
static void gamecom_pio_w(uint16_t offset, uint8_t data)   /* offset 0x14-0x17 */
{
	m_p_ram[offset] = data;
	switch (offset) {
		case SM8521_P1:
		case SM8521_P2:
			handle_input_press(m_p_ram[SM8521_P1] | (m_p_ram[SM8521_P2] << 8));
			break;
		case SM8521_P3:
			switch (data & 0xc0) {
				case 0x40: cart_ptr = cart_present ? cart_base : NULL; break;
				case 0x80: cart_ptr = NULL; break;   /* harness: only one cart slot */
				default:   cart_ptr = NULL; break;
			}
			return;
	}
}

static void gamecom_internal_w(uint16_t offset, uint8_t data)   /* offset 0x20-0x7F */
{
	switch (offset) {
	case SM8521_MMU0: break;
	case SM8521_MMU1: gamecom_set_mmu(1, data); break;
	case SM8521_MMU2: gamecom_set_mmu(2, data); break;
	case SM8521_MMU3: gamecom_set_mmu(3, data); break;
	case SM8521_MMU4: gamecom_set_mmu(4, data); break;

	case SM8521_DMBR: data &= 0x7f; break;

	case SM8521_TM0D: m_timer[0].upcounter_max = data; data = 0; break;
	case SM8521_TM0C:
		m_timer[0].enabled = BIT(data, 7);
		m_timer[0].prescale_max = gamecom_timer_limit[data & 0x07] >> 1;
		m_timer[0].prescale_count = 0;
		m_p_ram[SM8521_TM0D] = 0;
		break;
	case SM8521_TM1D: m_timer[1].upcounter_max = data; data = 0; break;
	case SM8521_TM1C:
		m_timer[1].enabled = BIT(data, 7);
		m_timer[1].prescale_max = gamecom_timer_limit[data & 0x07] >> 1;
		m_timer[1].prescale_count = 0;
		m_p_ram[SM8521_TM1D] = 0;
		break;

	case SM8521_CLKT:
		if (data & 0x80) {
			clock_enabled = 1;
			clock_minutes = (data & 0x40) ? 1 : 0;
			clock_frame_acc = 0;
		} else {
			clock_enabled = 0;
			data &= 0xC0;
		}
		break;

	case SM8521_LCDC: case SM8521_LCH: case SM8521_LCV:
		/* timing is driven manually by gamecom_run_frame */
		break;

	/* sound (stored only) */
	case SM8521_SGC:  m_sound.sgc  = data; break;
	case SM8521_SG0L: m_sound.sg0l = data; break;
	case SM8521_SG1L: m_sound.sg1l = data; break;
	case SM8521_SG0TH: m_sound.sg0t = (m_sound.sg0t & 0xFF)   | (data << 8); break;
	case SM8521_SG0TL: m_sound.sg0t = (m_sound.sg0t & 0xFF00) | data;        break;
	case SM8521_SG1TH: m_sound.sg1t = (m_sound.sg1t & 0xFF)   | (data << 8); break;
	case SM8521_SG1TL: m_sound.sg1t = (m_sound.sg1t & 0xFF00) | data;        break;
	case SM8521_SG2L: m_sound.sg2l = data; break;
	case SM8521_SG2TH: m_sound.sg2t = (m_sound.sg2t & 0xFF)   | (data << 8); break;
	case SM8521_SG2TL: m_sound.sg2t = (m_sound.sg2t & 0xFF00) | data;        break;
	case SM8521_SGDA:
		m_sound.sgda = data;
		if ((m_sound.sgc & 0x8f) == 0x88 && dac_count < GC_DAC_FIFO)
			dac_fifo[dac_count++] = data;   /* digitized sample stream (DAC) */
		break;
	default:
		if (offset >= SM8521_SG0W0 && offset <= SM8521_SG0W15)
			m_sound.sg0w[offset - SM8521_SG0W0] = data;
		else if (offset >= SM8521_SG1W0 && offset <= SM8521_SG1W15)
			m_sound.sg1w[offset - SM8521_SG1W0] = data;
		break;
	}
	m_p_ram[offset] = data;
}

/* ============================ program bus ============================ */
#ifdef GC_PROFILE
unsigned long gc_prof_reads=0, gc_prof_writes=0, gc_prof_dma_calls=0, gc_prof_dma_px=0;
#endif
uint8_t gc_program_read(uint16_t addr)
{
#ifdef GC_PROFILE
	gc_prof_reads++;
#endif
	if (addr < 0x0400) return ram[addr];        /* RAM + register file (pure reads) */
	if (addr < 0x1000) return 0;                /* unmapped */
	if (addr < 0x2000) return internal_rom ? internal_rom[addr - 0x1000] : 0;
	if (addr < 0x4000) return bank1[addr - 0x2000];
	if (addr < 0x6000) return bank2[addr - 0x4000];
	if (addr < 0x8000) return bank3[addr - 0x6000];
	if (addr < 0xA000) return bank4[addr - 0x8000];
	if (addr < 0xE000) return 0;                /* VRAM write-only, reads as 0 */
	return nvram[addr - 0xE000];
}

void gc_program_write(uint16_t addr, uint8_t data)
{
#ifdef GC_PROFILE
	gc_prof_writes++;
#endif
	if (addr < 0x0400) {
		if (addr >= 0x14 && addr <= 0x17)      gamecom_pio_w(addr, data);
		else if (addr >= 0x20 && addr <= 0x7F) gamecom_internal_w(addr, data);
		else                                   ram[addr] = data;
		return;
	}
	if (addr < 0xA000) return;                  /* ROM / banks: read-only */
	if (addr < 0xE000) { videoram[addr - 0xA000] = data; return; }
	nvram[addr - 0xE000] = data;
}

/* ============================ blitter DMA ============================ */
void gc_dma_cb(uint8_t cycles)
{
	(void)cycles;
	uint8_t dmc = m_p_ram[SM8521_DMC];
	if (!BIT(dmc, 7)) return;

	m_dma.overwrite_mode = BIT(dmc, 0);
	m_dma.transfer_mode  = dmc & 0x06;
	m_dma.adjust_x       = BIT(dmc, 3) ? -1 : 1;
	m_dma.decrement_y    = BIT(dmc, 4);

	m_dma.block_width  = m_p_ram[SM8521_DMDX];
	m_dma.block_height = m_p_ram[SM8521_DMDY];
	m_dma.source_x = m_p_ram[SM8521_DMX1];
	m_dma.source_x_current = m_dma.source_x & 3;
	m_dma.source_y = m_p_ram[SM8521_DMY1];
	m_dma.source_width = (m_p_ram[SM8521_LCH] & 0x20) ? 50 : 40;
	m_dma.dest_width = m_dma.source_width;
	m_dma.dest_x = m_p_ram[SM8521_DMX2];
	m_dma.dest_x_current = m_dma.dest_x & 3;
	m_dma.dest_y = m_p_ram[SM8521_DMY2];
	m_dma.palette = m_p_ram[SM8521_DMPL];
	m_dma.source_mask = 0x1FFF;
	m_dma.dest_mask = 0x1FFF;
	m_dma.source_bank = &videoram[BIT(m_p_ram[SM8521_DMVP], 0) ? 0x2000 : 0x0000];
	m_dma.dest_bank   = &videoram[BIT(m_p_ram[SM8521_DMVP], 1) ? 0x2000 : 0x0000];

	switch (m_dma.transfer_mode) {
	case 0x00: break;                                   /* VRAM->VRAM */
	case 0x02:                                          /* ROM->VRAM */
		m_dma.source_width = 64;
		m_dma.source_mask = 0x3FFF;
		if (m_p_ram[SM8521_DMBR] < 16)
			m_dma.source_bank = kernel_window((uint32_t)m_p_ram[SM8521_DMBR] << 14);
		else if (cart_ptr)
			m_dma.source_bank = cart_window((uint32_t)m_p_ram[SM8521_DMBR] << 14);
		break;
	case 0x04:                                          /* ExtRAM->VRAM */
		m_dma.source_width = 64;
		m_dma.source_bank = &nvram[0x0000];
		break;
	case 0x06:                                          /* VRAM->ExtRAM */
		m_dma.dest_width = 64;
		m_dma.dest_bank = &nvram[0x0000];
		break;
	}
#ifdef GC_PROFILE
	gc_prof_dma_calls++; gc_prof_dma_px += (unsigned long)(m_dma.block_width+1)*(m_dma.block_height+1);
#endif
	m_dma.source_current = m_dma.source_width * m_dma.source_y + (m_dma.source_x >> 2);
	m_dma.dest_current   = m_dma.dest_width   * m_dma.dest_y   + (m_dma.dest_x   >> 2);
	m_dma.source_line = m_dma.source_current;
	m_dma.dest_line   = m_dma.dest_current;

	for (uint16_t y = 0; y <= m_dma.block_height; y++) {
		for (uint16_t x = 0; x <= m_dma.block_width; x++) {
			uint16_t src_addr = m_dma.source_current & m_dma.source_mask;
			uint16_t dst_addr = m_dma.dest_current & m_dma.dest_mask;
			uint8_t dst_adj = (m_dma.dest_x_current ^ 3) << 1;
			uint8_t src_adj = (m_dma.source_x_current ^ 3) << 1;
			uint8_t source_pixel = (m_dma.source_bank[src_addr] >> src_adj) & 3;

			if (m_dma.overwrite_mode || source_pixel) {
				uint8_t other = m_dma.dest_bank[dst_addr] & ~(3 << dst_adj);
				m_dma.dest_bank[dst_addr] = other | (((m_dma.palette >> (source_pixel << 1)) & 3) << dst_adj);
			}

			m_dma.source_x_current += m_dma.adjust_x;
			if (BIT(m_dma.source_x_current, 2)) {
				m_dma.source_current += m_dma.adjust_x;
				m_dma.source_x_current &= 3;
			}
			m_dma.dest_x_current++;
			if (BIT(m_dma.dest_x_current, 2)) {
				m_dma.dest_current++;
				m_dma.dest_x_current &= 3;
			}
		}
		m_dma.source_x_current = m_dma.source_x & 3;
		m_dma.dest_x_current = m_dma.dest_x & 3;
		if (m_dma.decrement_y) m_dma.source_line -= m_dma.source_width;
		else                   m_dma.source_line += m_dma.source_width;
		m_dma.source_current = m_dma.source_line;
		m_dma.dest_line += m_dma.dest_width;
		m_dma.dest_current = m_dma.dest_line;
	}

	m_p_ram[SM8521_DMC] &= 0x7f;
	sm8500_set_input(SM8500_DMA_INT, SM8500_ASSERT_LINE);
}

/* ============================ hardware timers ============================ */
void gc_timer_cb(uint8_t data)
{
	if (m_timer[0].enabled) {
		m_timer[0].prescale_count += data;
		while (m_timer[0].prescale_count >= m_timer[0].prescale_max) {
			m_timer[0].prescale_count -= m_timer[0].prescale_max;
			m_p_ram[SM8521_TM0D]++;
			if (m_p_ram[SM8521_TM0D] >= m_timer[0].upcounter_max) {
				m_p_ram[SM8521_TM0D] = 0;
				if (BIT(m_p_ram[SM8521_IE0], 6) && BIT(m_p_ram[SM8521_PS1], 0))
					sm8500_set_input(SM8500_TIM0_INT, SM8500_ASSERT_LINE);
			}
		}
	}
	if (m_timer[1].enabled) {
		m_timer[1].prescale_count += data;
		while (m_timer[1].prescale_count >= m_timer[1].prescale_max) {
			m_timer[1].prescale_count -= m_timer[1].prescale_max;
			m_p_ram[SM8521_TM1D]++;
			if (m_p_ram[SM8521_TM1D] >= m_timer[1].upcounter_max) {
				m_p_ram[SM8521_TM1D] = 0;
				if (BIT(m_p_ram[SM8521_IE1], 6) && BIT(m_p_ram[SM8521_PS1], 0) && ((m_p_ram[SM8521_PS0] & 7) < 4))
					sm8500_set_input(SM8500_TIM1_INT, SM8500_ASSERT_LINE);
			}
		}
	}
}

/* ============================ video ============================ */
static void render_scanline(int s)
{
	uint16_t base = (m_p_ram[SM8521_LCDC] & 0x40) ? 0x2000 : 0x0000;

	if (~m_p_ram[SM8521_LCDC] & 0x80) {       /* display off: black column */
		for (int y = 0; y < GAMECOM_H; y++)
			gamecom_fb[y * GAMECOM_W + s] = 0;
		return;
	}

	const uint8_t *line = &videoram[base + 40 * s];
	int pal[4];
	switch (m_p_ram[SM8521_LCDC] & 0x30) {
		case 0x00: pal[0]=4; pal[1]=3; pal[2]=2; pal[3]=0; break;
		case 0x10: pal[0]=4; pal[1]=3; pal[2]=1; pal[3]=0; break;
		case 0x20: pal[0]=4; pal[1]=3; pal[2]=1; pal[3]=0; break;
		default:   pal[0]=4; pal[1]=2; pal[2]=1; pal[3]=0; break;  /* 0x30 */
	}
	for (int i = 0; i < 40; i++) {
		uint8_t p = line[i];
		gamecom_fb[(i*4+0) * GAMECOM_W + s] = pal[(p >> 6) & 3];
		gamecom_fb[(i*4+1) * GAMECOM_W + s] = pal[(p >> 4) & 3];
		gamecom_fb[(i*4+2) * GAMECOM_W + s] = pal[(p >> 2) & 3];
		gamecom_fb[(i*4+3) * GAMECOM_W + s] = pal[(p     ) & 3];
	}
}

/* ============================ frame ============================ */
void gamecom_run_frame(void)
{
	const int slice = CYCLES_PER_FRAME / Y_PIXELS;
	for (m_scanline = 0; m_scanline < Y_PIXELS; m_scanline++) {
		render_scanline(m_scanline);
		sm8500_execute(slice);
	}
	/* vblank: LCD controller interrupt, once per frame */
	sm8500_set_input(SM8500_LCDC_INT, SM8500_ASSERT_LINE);

	/* coarse real-time clock: tick CLKT + CK_INT */
	if (clock_enabled) {
		int period = clock_minutes ? 3600 : 60;   /* frames per tick */
		if (++clock_frame_acc >= period) {
			clock_frame_acc = 0;
			uint8_t v = (m_p_ram[SM8521_CLKT] + 1) & 0x3f;
			m_p_ram[SM8521_CLKT] = (m_p_ram[SM8521_CLKT] & 0xC0) | v;
			sm8500_set_input(SM8500_CK_INT, SM8500_ASSERT_LINE);
		}
	}
}

/* ============================ audio ============================ */
/* SG0/SG1 are 32-step (16 byte x 2 nibble) wavetable oscillators clocked at
 * 2,764,800 / sgXt steps per second (MAME gamecom_sound{0,1}_timer_callback),
 * i.e. a tone of 2764800/(32*sgXt) Hz, amplitude sgXl/31. SGC: bit7 master,
 * bit0 SG0, bit1 SG1, bit3 DAC. The DAC (8-bit SGDA, used for digitized FX) is
 * mixed as its held level — imperfect, like MAME, but audible. SG2 noise TODO. */
void gamecom_audio_mix(int16_t *out, int n, int sample_rate)
{
	uint8_t sgc = m_sound.sgc;
	int     master = (sgc & 0x80) != 0;
	float   inc0 = (master && (sgc & 0x01) && m_sound.sg0t) ? (2764800.0f / m_sound.sg0t) / sample_rate : 0.0f;
	float   inc1 = (master && (sgc & 0x02) && m_sound.sg1t) ? (2764800.0f / m_sound.sg1t) / sample_rate : 0.0f;
	int     dn = dac_count;   /* DAC samples captured during the frame just run */

	for (int i = 0; i < n; i++) {
		int s = 0;
		if (inc0 > 0.0f) {
			int st = (int)sg0_phase & 31;
			int nib = (m_sound.sg0w[st >> 1] >> ((st & 1) * 4)) & 0xf;
			s += (nib - 8) * m_sound.sg0l;
			sg0_phase += inc0;
			if (sg0_phase >= 32.0f) sg0_phase -= 32.0f;
		}
		if (inc1 > 0.0f) {
			int st = (int)sg1_phase & 31;
			int nib = (m_sound.sg1w[st >> 1] >> ((st & 1) * 4)) & 0xf;
			s += (nib - 8) * m_sound.sg1l;
			sg1_phase += inc1;
			if (sg1_phase >= 32.0f) sg1_phase -= 32.0f;
		}
		s *= 40;                                  /* SG headroom: 2ch*8*31 ≈ 496 -> ~20k */

		if (dn > 0) {                             /* resample the DAC stream across the frame */
			int d = (int)dac_fifo[(i * dn) / n] - 128;
			s += d * 110;                         /* 8-bit center -> ~14k peak */
		}

		if (s > 32767) s = 32767;
		if (s < -32768) s = -32768;
		out[i] = (int16_t)s;
	}
	dac_count = 0;   /* consumed; next frame refills */
}

/* ============================ init / reset ============================ */
static void load_palette(void)
{
	static const uint8_t pal[5][3] = {
		{0x00,0x00,0x00}, {0x0f,0x4f,0x2f}, {0x6f,0x8f,0x4f},
		{0x8f,0xcf,0x8f}, {0xdf,0xff,0x8f}
	};
	memcpy(gamecom_palette, pal, sizeof(pal));
}

int gamecom_init(const uint8_t *irom, int irom_len,
                 const uint8_t *krom, int krom_len,
                 const uint8_t *cart, int cart_len)
{
	memset(ram, 0, sizeof(ram));
	memset(videoram, 0, sizeof(videoram));
	memset(nvram, 0, sizeof(nvram));
	memset(gc_zero_page, 0, sizeof(gc_zero_page));
	memset(gamecom_fb, 0, sizeof(gamecom_fb));
	load_palette();

	if (!irom || irom_len < 0x1000) return 1;   /* internal BIOS (4KB) is mandatory */
	internal_rom = irom;

	kernel_rom  = (krom && krom_len > 0) ? krom : NULL;
	kernel_size = (krom && krom_len > 0) ? (uint32_t)(krom_len > 0x40000 ? 0x40000 : krom_len) : 0;

	/* Cartridge is referenced in place; cart_window() maps bank/DMA offsets onto
	 * it the way MAME's mirrored 2MB image would (no physical copy). */
	cart_base    = (cart && cart_len > 0) ? cart : NULL;
	cart_size    = (cart && cart_len > 0) ? (uint32_t)(cart_len > 0x200000 ? 0x200000 : cart_len) : 0;
	cart_present = cart_size > 0;

	/* machine_reset */
	bank1 = bank2 = bank3 = bank4 = kernel_rom ? kernel_rom : gc_zero_page;
	cart_ptr = NULL;
	memset(&m_dma, 0, sizeof(m_dma));
	memset(m_timer, 0, sizeof(m_timer));
	memset(&m_sound, 0, sizeof(m_sound));
	clock_enabled = clock_minutes = clock_frame_acc = 0;
	m_scanline = 0;
	sg0_phase = sg1_phase = 0.0f;
	dac_count = 0;
	for (int c = 0; c < GC_GRID_COLS; c++) io_grid[c] = 0;
	io_in0 = io_in1 = io_in2 = 0xFF;

	sm8500_reset();
	return 0;
}

int gamecom_state_rw(gc_rw_fn fn, void *ctx, int saving)
{
	/* m_dma is transient (rebuilt at the start of every blit) and holds raw
	 * pointers, so it is intentionally not serialized. bank1-4 / cart_ptr are
	 * re-derived from the restored registers below. */
	uint8_t cart_flag = (cart_ptr != NULL);
	int ok = fn(ctx, ram, sizeof ram)
	      && fn(ctx, videoram, sizeof videoram)
	      && fn(ctx, nvram, sizeof nvram)
	      && fn(ctx, &io_in0, 1) && fn(ctx, &io_in1, 1) && fn(ctx, &io_in2, 1)
	      && fn(ctx, io_grid, sizeof io_grid)
	      && fn(ctx, &m_scanline, sizeof m_scanline)
	      && fn(ctx, m_timer, sizeof m_timer)
	      && fn(ctx, &m_sound, sizeof m_sound)
	      && fn(ctx, &clock_enabled, sizeof clock_enabled)
	      && fn(ctx, &clock_minutes, sizeof clock_minutes)
	      && fn(ctx, &clock_frame_acc, sizeof clock_frame_acc)
	      && fn(ctx, &cart_flag, 1)
	      && sm8500_state_rw(fn, ctx);
	if (!ok) return 0;

	if (!saving) {
		cart_ptr = cart_flag ? cart_base : NULL;
		gamecom_set_mmu(1, ram[SM8521_MMU1]);
		gamecom_set_mmu(2, ram[SM8521_MMU2]);
		gamecom_set_mmu(3, ram[SM8521_MMU3]);
		gamecom_set_mmu(4, ram[SM8521_MMU4]);
	}
	return 1;
}
