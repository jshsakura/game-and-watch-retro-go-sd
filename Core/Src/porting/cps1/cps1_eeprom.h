#pragma once
/*
 * 93C46 serial EEPROM, as wired on CPS-1 QSound boards.
 *
 * WHY THIS EXISTS: Tenchi wo Kurau II does not get past its boot sequence
 * without one. The port's first real-ROM run found the main CPU reading
 * 0xF1C006 197,630 times in 600 frames -- three orders of magnitude more
 * than any other address -- and the driver behind it (0xD464) is a plain
 * bit-banger:
 *
 *      00d464:  ori.w   #$40, d2
 *      00d468:  move.w  d2, $f1c006.l      ; CLK high
 *      00d46e:  andi.w  #$ffbf, d2
 *      00d472:  move.w  d2, $f1c006.l      ; CLK low
 *
 * Answering every read with a constant (the earlier stub returned 0xFFFF)
 * lets boot continue but feeds the game a garbage bit stream, and it never
 * reaches a drawing state.
 *
 * PIN MAPPING -- confirmed in MAME (capcom/cps1.cpp, the EEPROMIN/EEPROMOUT
 * port definitions at the qsound input-port block), not guessed:
 *
 *   read  0xF1C006   bit 0  DO   (data out of the EEPROM)
 *   write 0xF1C006   bit 0  DI   (data in)
 *                    bit 6  CLK
 *                    bit 7  CS
 *
 * ORGANISATION: 93C46 with ORG tied for 16-bit words -- 64 words of 16
 * bits. Instruction frame is 9 bits, MSB first: a start bit (1), a 2-bit
 * opcode, then a 6-bit address.
 *
 *   10 aaaaaa   READ   -- DO emits one dummy 0 then D15..D0
 *   01 aaaaaa   WRITE  -- 16 more bits are clocked in, then stored
 *   11 aaaaaa   ERASE  -- word := 0xFFFF
 *   00 11xxxx   EWEN   -- enable writes/erases
 *   00 00xxxx   EWDS   -- disable them (power-on default)
 *   00 10xxxx   ERAL   -- erase all
 *   00 01xxxx   WRAL   -- write all with the next 16 clocked-in bits
 *
 * Writes and erases are ignored unless EWEN was issued, exactly as the real
 * part behaves -- a game that forgets to enable and still expects its data
 * to stick would then misbehave identically here, which is the point.
 */
#include <stdint.h>

#define CPS1_EEPROM_WORDS 64

/* Bit positions in the 0xF1C006 port. */
#define CPS1_EEPROM_DI_BIT  0x0001u
#define CPS1_EEPROM_CLK_BIT 0x0040u
#define CPS1_EEPROM_CS_BIT  0x0080u

typedef struct {
    uint16_t data[CPS1_EEPROM_WORDS];

    uint8_t  cs, clk;          /* previous pin levels, for edge detection */
    uint8_t  do_bit;           /* what a read of the port currently returns */

    uint8_t  phase;            /* 0 = shifting in a command, 1 = read out,
                                * 2 = shifting in write data */
    uint8_t  write_enabled;

    uint32_t cmd_shift;        /* command bits accumulated so far */
    uint8_t  cmd_bits;

    uint16_t io_shift;         /* data being shifted out (read) or in (write) */
    uint8_t  io_bits;
    uint8_t  addr;
    uint8_t  wral;             /* the pending write-data frame is a WRAL */
} cps1_eeprom_t;

/* Blank part: every word 0xFFFF, writes disabled, DO idle high. A game that
 * finds this pattern is expected to take its "settings invalid, write
 * defaults" path, which is what an unprogrammed board does. */
void cps1_eeprom_reset(cps1_eeprom_t *e);

/* Value to return for a read of 0xF1C006. Only bit 0 is meaningful; the
 * rest of the word floats high on the real board. */
uint16_t cps1_eeprom_read_port(const cps1_eeprom_t *e);

/* Feed a write of 0xF1C006. All protocol state advances from the CLK
 * rising edge and the CS level, so callers only have to pass the value
 * through. */
void cps1_eeprom_write_port(cps1_eeprom_t *e, uint16_t value);
