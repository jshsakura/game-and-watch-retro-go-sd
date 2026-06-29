// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
//
// Freestanding C port of MAME's sm8500.cpp.  Member variables become file-scope
// globals using the EXACT MAME names so that sm85ops.h compiles verbatim.

#include "sm8500.h"

/* logerror is only used in commented-out / diagnostic spots of the opcode table */
#define logerror(...) ((void)0)
#ifndef BIT
#define BIT(x,n) (((x) >> (n)) & 1)
#endif

/* Flags */
#define FLAG_C 0x80
#define FLAG_Z 0x40
#define FLAG_S 0x20
#define FLAG_V 0x10
#define FLAG_D 0x08
#define FLAG_H 0x04
#define FLAG_B 0x02
#define FLAG_I 0x01

/* Interrupt line aliases used by the ported code (MAME names) */
#define ILL_INT  SM8500_ILL_INT
#define DMA_INT  SM8500_DMA_INT
#define TIM0_INT SM8500_TIM0_INT
#define EXT_INT  SM8500_EXT_INT
#define UART_INT SM8500_UART_INT
#define LCDC_INT SM8500_LCDC_INT
#define TIM1_INT SM8500_TIM1_INT
#define CK_INT   SM8500_CK_INT
#define PIO_INT  SM8500_PIO_INT
#define WDT_INT  SM8500_WDT_INT
#define NMI_INT  SM8500_NMI_INT

#define ASSERT_LINE SM8500_ASSERT_LINE
#define CLEAR_LINE  SM8500_CLEAR_LINE

static const uint8_t sm8500_b2w[8] = { 0, 8, 2, 10, 4, 12, 6, 14 };

/* --- CPU state (MAME member names) --- */
static uint16_t m_PC;
static uint8_t  m_IE0, m_IE1, m_IR0, m_IR1;
static uint8_t  m_SYS, m_CKC, m_clock_changed;
static uint16_t m_SP;
static uint8_t  m_PS0, m_PS1;
static uint16_t m_IFLAGS;
static uint8_t  m_CheckInterrupts;
static int      m_halted;
static int      m_icount;
static uint16_t m_oldpc;
static uint8_t  m_register_ram[0x108];

uint16_t sm8500_pc(void) { return m_oldpc; }

static void get_sp(void)
{
	m_SP = gc_program_read(0x1d);
	if (m_SYS & 0x40) m_SP |= (gc_program_read(0x1c) << 8);
}

static uint8_t mem_readbyte(uint32_t offset)
{
	offset &= 0xffff;
	if (offset < 0x10)
		return m_register_ram[offset + (m_PS0 & 0xF8)];
	return gc_program_read((uint16_t)offset);
}

static void mem_writebyte(uint32_t offset, uint8_t data)
{
	uint8_t i;
	offset &= 0xffff;
	if (offset < 0x10)
		m_register_ram[offset + (m_PS0 & 0xF8)] = data;

	gc_program_write((uint16_t)offset, data);

	switch (offset)
	{
		case 0x10: m_IE0 = data; break;
		case 0x11: m_IE1 = data; break;
		case 0x12: m_IR0 = data; break;
		case 0x13: m_IR1 = data; break;
		case 0x19: m_SYS = data; break;
		case 0x1a: m_CKC = data; break;
		case 0x1c:
		case 0x1d: get_sp(); break;
		case 0x1e: m_PS0 = data;
			for (i = 0; i < 16; i++)   /* mirror register file into low RAM */
				gc_program_write(i, mem_readbyte(i));
			break;
		case 0x1f: m_PS1 = data; break;
	}
}

static inline uint16_t mem_readword(uint32_t address)
{
	return (mem_readbyte(address) << 8) | mem_readbyte(address + 1);
}

static inline void mem_writeword(uint32_t address, uint16_t value)
{
	mem_writebyte(address, value >> 8);
	mem_writebyte(address + 1, value);
}

#define PUSH_BYTE(X)    m_SP--; \
		if ((m_SYS & 0x40) == 0) m_SP &= 0xFF; \
		mem_writebyte(m_SP, X);

static void take_interrupt(uint16_t vector)
{
	get_sp();
	m_SYS = gc_program_read(0x19);
	m_PS1 = gc_program_read(0x1f);
	PUSH_BYTE(m_PC & 0xFF);
	PUSH_BYTE(m_PC >> 8);
	PUSH_BYTE(m_PS1);
	m_PS1 &= ~0x01;
	gc_program_write(0x1f, m_PS1);
	gc_program_write(0x1d, m_SP & 0xFF);
	if (m_SYS & 0x40) gc_program_write(0x1c, m_SP >> 8);
	m_PC = mem_readword(vector);
}

static void process_interrupts(void)
{
	if (!m_CheckInterrupts) return;

	int irqline = 0;
	while (irqline < 11)
	{
		if (m_IFLAGS & (1 << irqline))
		{
			m_halted = 0;
			m_IE0 = gc_program_read(0x10);
			m_IE1 = gc_program_read(0x11);
			m_IR0 = gc_program_read(0x12);
			m_IR1 = gc_program_read(0x13);
			m_PS0 = gc_program_read(0x1e);
			m_PS1 = gc_program_read(0x1f);
			switch (irqline)
			{
			case WDT_INT:
				take_interrupt(0x101C);
				break;
			case ILL_INT:
			case NMI_INT:
				take_interrupt(0x101E);
				break;
			case DMA_INT:
				m_IR0 |= 0x80;
				if (BIT(m_IE0, 7) && BIT(m_PS1, 0))
					take_interrupt(0x1000);
				break;
			case TIM0_INT:
				m_IR0 |= 0x40;
				if (BIT(m_IE0, 6) && BIT(m_PS1, 0))
					take_interrupt(0x1002);
				break;
			case EXT_INT:
				m_IR0 |= 0x10;
				if (BIT(m_IE0, 4) && ((m_PS0 & 0x07) < 7) && BIT(m_PS1, 0))
					take_interrupt(0x1006);
				break;
			case UART_INT:
				m_IR0 |= 0x08;
				if (BIT(m_IE0, 3) && ((m_PS0 & 0x07) < 6) && BIT(m_PS1, 0))
					take_interrupt(0x1008);
				break;
			case LCDC_INT:
				m_IR0 |= 0x01;
				if (BIT(m_IE0, 0) && ((m_PS0 & 0x07) < 5) && BIT(m_PS1, 0))
					take_interrupt(0x100E);
				break;
			case TIM1_INT:
				m_IR1 |= 0x40;
				if (BIT(m_IE1, 6) && ((m_PS0 & 0x07) < 4) && BIT(m_PS1, 0))
					take_interrupt(0x1012);
				break;
			case CK_INT:
				m_IR1 |= 0x10;
				if (BIT(m_IE1, 4) && ((m_PS0 & 0x07) < 3) && BIT(m_PS1, 0))
					take_interrupt(0x1016);
				break;
			case PIO_INT:
				m_IR1 |= 0x04;
				if (BIT(m_IE1, 2) && ((m_PS0 & 0x07) < 2) && BIT(m_PS1, 0))
					take_interrupt(0x101A);
				break;
			}
			m_IFLAGS &= ~(1 << irqline);
			gc_program_write(0x12, m_IR0);
			gc_program_write(0x13, m_IR1);
		}
		irqline++;
	}
}

void sm8500_reset(void)
{
	for (int i = 0; i < 0x108; i++) m_register_ram[i] = 0;

	m_PC = 0x1020;
	m_clock_changed = 0;
	m_CheckInterrupts = 0;
	m_halted = 0;
	m_IFLAGS = 0;
	mem_writeword(0x10, 0);          /* IE0, IE1 */
	mem_writeword(0x12, 0);          /* IR0, IR1 */
	mem_writeword(0x14, 0xffff);     /* P0, P1 */
	mem_writeword(0x16, 0xff00);     /* P2, P3 */
	mem_writebyte(0x19, 0);          /* SYS */
	mem_writebyte(0x1a, 0);          /* CKC */
	mem_writebyte(0x1f, 0);          /* PS1 */
	mem_writebyte(0x2b, 0xff);       /* URTT */
	mem_writebyte(0x2d, 0x42);       /* URTS */
	mem_writebyte(0x5f, 0x38);       /* WDTC */
}

int sm8500_execute(int cycles)
{
	m_icount = cycles;
	do
	{
		int       mycycles = 0;
		uint8_t   r1, r2;
		uint16_t  s1, s2;
		uint32_t  d1, d2;
		uint32_t  res;
		(void)s1; (void)d1; (void)d2; (void)res;  /* some opcodes leave these unused */

		process_interrupts();
		if (!m_halted) {
			m_oldpc = m_PC;
			uint8_t op = mem_readbyte(m_PC++);
			m_SYS = gc_program_read(0x19);
			m_PS0 = gc_program_read(0x1e);
			m_PS1 = gc_program_read(0x1f);
			get_sp();
			switch (op)
			{
#include "sm85ops.h"
			}
			if (m_SYS & 0x40) gc_program_write(0x1c, m_SP >> 8);
			gc_program_write(0x1d, m_SP & 0xFF);
			mem_writebyte(0x1e, m_PS0);
			gc_program_write(0x1f, m_PS1);
		} else {
			mycycles = 4;
			gc_dma_cb(mycycles);
		}
		gc_timer_cb(mycycles);
		m_icount -= mycycles;
	} while (m_icount > 0);

	return cycles - m_icount;
}

void sm8500_set_input(int inptnum, int state)
{
	m_IR0 = gc_program_read(0x12);
	m_IR1 = gc_program_read(0x13);
	if (state == ASSERT_LINE)
	{
		m_IFLAGS |= (0x01 << inptnum);
		m_CheckInterrupts = 1;
		switch (inptnum)
		{
			case DMA_INT:   m_IR0 |= 0x80; break;
			case TIM0_INT:  m_IR0 |= 0x40; break;
			case EXT_INT:   m_IR0 |= 0x10; break;
			case UART_INT:  m_IR0 |= 0x08; break;
			case LCDC_INT:  m_IR0 |= 0x01; break;
			case TIM1_INT:  m_IR1 |= 0x40; break;
			case CK_INT:    m_IR1 |= 0x10; break;
			case PIO_INT:   m_IR1 |= 0x04; break;
		}
	}
	else
	{
		m_IFLAGS &= ~(0x01 << inptnum);
		switch (inptnum)
		{
			case DMA_INT:   m_IR0 &= ~0x80; break;
			case TIM0_INT:  m_IR0 &= ~0x40; break;
			case EXT_INT:   m_IR0 &= ~0x10; break;
			case UART_INT:  m_IR0 &= ~0x08; break;
			case LCDC_INT:  m_IR0 &= ~0x01; break;
			case TIM1_INT:  m_IR1 &= ~0x40; break;
			case CK_INT:    m_IR1 &= ~0x10; break;
			case PIO_INT:   m_IR1 &= ~0x04; break;
		}
		if (0 == m_IFLAGS)
			m_CheckInterrupts = 0;
	}
	gc_program_write(0x12, m_IR0);
	gc_program_write(0x13, m_IR1);
}

int sm8500_state_rw(int (*fn)(void *ctx, void *data, uint32_t len), void *ctx)
{
	return fn(ctx, &m_PC, sizeof m_PC)
	    && fn(ctx, &m_IE0, 1) && fn(ctx, &m_IE1, 1)
	    && fn(ctx, &m_IR0, 1) && fn(ctx, &m_IR1, 1)
	    && fn(ctx, &m_SYS, 1) && fn(ctx, &m_CKC, 1)
	    && fn(ctx, &m_clock_changed, 1)
	    && fn(ctx, &m_SP, sizeof m_SP)
	    && fn(ctx, &m_PS0, 1) && fn(ctx, &m_PS1, 1)
	    && fn(ctx, &m_IFLAGS, sizeof m_IFLAGS)
	    && fn(ctx, &m_CheckInterrupts, 1)
	    && fn(ctx, &m_halted, sizeof m_halted)
	    && fn(ctx, m_register_ram, sizeof m_register_ram);
}
