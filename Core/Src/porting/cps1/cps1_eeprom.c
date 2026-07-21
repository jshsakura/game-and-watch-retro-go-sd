/* 93C46 serial EEPROM for CPS-1 QSound boards. See cps1_eeprom.h for the
 * pin mapping, the instruction set, and why the port needs one at all. */
#include "cps1_eeprom.h"

#define CMD_FRAME_BITS 9u   /* start bit + 2 opcode bits + 6 address bits */

void cps1_eeprom_reset(cps1_eeprom_t *e)
{
    for (unsigned i = 0; i < CPS1_EEPROM_WORDS; i++)
        e->data[i] = 0xFFFFu;
    e->cs = e->clk = 0;
    e->do_bit = 1;            /* idle high = "ready", same as the real part */
    e->phase = 0;
    e->write_enabled = 0;
    e->cmd_shift = 0;
    e->cmd_bits = 0;
    e->io_shift = 0;
    e->io_bits = 0;
    e->addr = 0;
    e->wral = 0;
}

uint16_t cps1_eeprom_read_port(const cps1_eeprom_t *e)
{
    /* Only DO is driven; the rest of the word is pulled high on the board. */
    return (uint16_t)(0xFFFEu | (e->do_bit & 1u));
}

static void eeprom_begin_command(cps1_eeprom_t *e)
{
    /* cmd_shift holds 9 bits: 1 SB, 2 opcode, 6 address. */
    unsigned op   = (e->cmd_shift >> 6) & 0x3u;
    unsigned addr = e->cmd_shift & 0x3Fu;

    e->addr = (uint8_t)addr;

    switch (op) {
    case 0x2: /* READ */
        e->io_shift = e->data[addr];
        e->io_bits = 16;
        e->phase = 1;
        e->do_bit = 0;   /* the dummy 0 that precedes D15 on a real 93C46 */
        return;

    case 0x1: /* WRITE -- 16 data bits follow */
        e->io_shift = 0;
        e->io_bits = 0;
        e->wral = 0;
        e->phase = 2;
        return;

    case 0x3: /* ERASE */
        if (e->write_enabled)
            e->data[addr] = 0xFFFFu;
        e->phase = 0;
        e->cmd_bits = 0;
        e->cmd_shift = 0;
        e->do_bit = 1;   /* ready */
        return;

    default:  /* op == 0: the address field selects the sub-command */
        switch ((addr >> 4) & 0x3u) {
        case 0x3: e->write_enabled = 1; break;              /* EWEN */
        case 0x0: e->write_enabled = 0; break;              /* EWDS */
        case 0x2:                                            /* ERAL */
            if (e->write_enabled)
                for (unsigned i = 0; i < CPS1_EEPROM_WORDS; i++)
                    e->data[i] = 0xFFFFu;
            break;
        case 0x1:                                            /* WRAL */
            e->io_shift = 0;
            e->io_bits = 0;
            e->wral = 1;
            e->phase = 2;
            return;
        }
        e->phase = 0;
        e->cmd_bits = 0;
        e->cmd_shift = 0;
        e->do_bit = 1;
        return;
    }
}

static void eeprom_clock_rising(cps1_eeprom_t *e, unsigned di)
{
    switch (e->phase) {
    case 0: /* shifting in a command frame */
        /* A real part ignores clocks until it sees the start bit, so leading
         * zeroes must not be allowed to fill the frame. */
        if (e->cmd_bits == 0 && di == 0)
            return;
        e->cmd_shift = (e->cmd_shift << 1) | di;
        e->cmd_bits++;
        if (e->cmd_bits >= CMD_FRAME_BITS)
            eeprom_begin_command(e);
        return;

    case 1: /* shifting a word out */
        e->do_bit = (uint8_t)((e->io_shift >> 15) & 1u);
        e->io_shift = (uint16_t)(e->io_shift << 1);
        if (e->io_bits)
            e->io_bits--;
        if (e->io_bits == 0) {
            /* Sequential read would continue into the next word; this part
             * is only ever used one word at a time here, so fall back to
             * waiting for a fresh command frame. */
            e->phase = 0;
            e->cmd_bits = 0;
            e->cmd_shift = 0;
        }
        return;

    default: /* case 2: shifting write data in */
        e->io_shift = (uint16_t)((e->io_shift << 1) | di);
        e->io_bits++;
        if (e->io_bits >= 16) {
            if (e->write_enabled) {
                if (e->wral)
                    for (unsigned i = 0; i < CPS1_EEPROM_WORDS; i++)
                        e->data[i] = e->io_shift;
                else
                    e->data[e->addr] = e->io_shift;
            }
            e->phase = 0;
            e->cmd_bits = 0;
            e->cmd_shift = 0;
            e->do_bit = 1;   /* write cycle finished -> ready */
        }
        return;
    }
}

void cps1_eeprom_write_port(cps1_eeprom_t *e, uint16_t value)
{
    unsigned cs  = (value & CPS1_EEPROM_CS_BIT)  ? 1u : 0u;
    unsigned clk = (value & CPS1_EEPROM_CLK_BIT) ? 1u : 0u;
    unsigned di  = (value & CPS1_EEPROM_DI_BIT)  ? 1u : 0u;

    if (!cs) {
        /* Deselect aborts whatever frame was in flight and releases DO. */
        e->phase = 0;
        e->cmd_bits = 0;
        e->cmd_shift = 0;
        e->io_bits = 0;
        e->do_bit = 1;
    } else if (clk && !e->clk) {
        eeprom_clock_rising(e, di);
    }

    e->cs = (uint8_t)cs;
    e->clk = (uint8_t)clk;
}
