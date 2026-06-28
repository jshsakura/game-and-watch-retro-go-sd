/* PC Engine CD-ROM2 SCSI target. See pce_scsi.h. Iteration 1: boot/data path. */
#include "pce_scsi.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "h6280.h"          /* CPU_PCE, INT_IRQ2 */

/* --- diagnostics: append the command stream to SD so we can see where the
 *     System Card stalls. APPEND-only: delete /pcecd_diag.txt before a clean
 *     test. Capped so it can't flood. --- */
#define PCECD_DIAG 1
#if PCECD_DIAG
static int s_diag_lines;
static void diag(const char *fmt, ...)
{
    if (s_diag_lines > 200) return;
    s_diag_lines++;
    FILE *f = fopen("/pcecd_diag.txt", "a");
    if (!f) return;
    va_list ap; __builtin_va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    __builtin_va_end(ap);
    fclose(f);
}
#else
#define diag(...) ((void)0)
#endif

/* $1800 bus-status bits (SCSI phase lines, as the System Card reads them). */
#define ST_BSY  0x80
#define ST_REQ  0x40
#define ST_MSG  0x20
#define ST_CD   0x10   /* Command/Data: 1 = command/status/message, 0 = data */
#define ST_IO   0x08   /* 1 = target->initiator (in), 0 = initiator->target (out) */

/* $1802 IRQ-enable / $1803 IRQ-status bits. */
#define IRQ_DATA_IN   0x20   /* data transfer ready */
#define IRQ_STATUS    0x40   /* status/command complete */

enum { PH_BUSFREE, PH_COMMAND, PH_DATAIN, PH_STATUS, PH_MSGIN };

static const pce_cd_toc_t *s_toc;
static bool     s_present;

static int      s_phase;
static uint8_t  s_cmd[16];
static int      s_cmd_len;     /* bytes expected for current CDB (6/10) */
static int      s_cmd_idx;
static uint8_t  s_status;      /* SCSI status byte (0 = GOOD) */
static uint8_t  s_message;

static uint8_t  s_sector[PCE_CD_SECTOR_RAW];
static int      s_data_ptr;    /* index into the 2048-byte user area */
static int      s_data_len;    /* valid user bytes in s_sector (2048) */
static uint32_t s_read_lba;
static uint32_t s_read_remain; /* sectors left to stream */

static uint8_t  s_irq_enable;
static uint8_t  s_irq_status;

static void raise_cd_irq(uint8_t flag)
{
    s_irq_status |= flag;
    if (s_irq_enable & flag)
        CPU_PCE.irq_lines |= INT_IRQ2;
}

void pce_scsi_set_disc(const pce_cd_toc_t *toc, bool present)
{
    s_toc = toc;
    s_present = present && toc && toc->num_tracks > 0;
    diag("MOUNT present=%d tracks=%d total_lba=%lu\n", s_present,
         toc ? toc->num_tracks : -1, (unsigned long)(toc ? toc->total_lba : 0));
    if (toc)
        for (int i = 0; i < toc->num_tracks && i < 6; i++)
            diag("  trk%d type=%d start=%lu len=%lu bin=%s\n",
                 toc->tracks[i].number, toc->tracks[i].type,
                 (unsigned long)toc->tracks[i].start_lba,
                 (unsigned long)toc->tracks[i].length_lba, toc->tracks[i].bin_path);
    pce_scsi_reset();
}

void pce_scsi_reset(void)
{
    s_phase = PH_BUSFREE;
    s_cmd_idx = s_cmd_len = 0;
    s_status = s_message = 0;
    s_data_ptr = s_data_len = 0;
    s_read_remain = 0;
    s_irq_status = 0;
}

/* Load the next data sector's 2048 user bytes (MODE1 payload at raw offset 16). */
static bool load_next_sector(void)
{
    if (!s_present || s_read_remain == 0)
        return false;
    if (!pce_cd_read_sector(s_toc, s_read_lba, s_sector))
        return false;
    s_data_ptr = 0;
    s_data_len = 2048;
    s_read_lba++;
    s_read_remain--;
    return true;
}

static void enter_status(uint8_t status)
{
    s_status = status;
    s_phase = PH_STATUS;
    raise_cd_irq(IRQ_STATUS);
}

/* Decode and start executing a completed CDB. */
static void execute_command(void)
{
    diag("CMD %02x %02x %02x %02x %02x %02x irqen=%02x\n",
         s_cmd[0], s_cmd[1], s_cmd[2], s_cmd[3], s_cmd[4], s_cmd[5], s_irq_enable);
    switch (s_cmd[0]) {
    case 0x00: /* TEST UNIT READY */
        enter_status(s_present ? 0x00 : 0x02);
        break;

    case 0x08: { /* READ(6): LBA in cmd[1..3], count in cmd[4] */
        uint32_t lba = ((uint32_t)(s_cmd[1] & 0x1F) << 16) | ((uint32_t)s_cmd[2] << 8) | s_cmd[3];
        uint32_t cnt = s_cmd[4] ? s_cmd[4] : 1;
        s_read_lba = lba;
        s_read_remain = cnt;
        bool ok = load_next_sector();
        diag("  READ lba=%lu cnt=%lu ok=%d\n", (unsigned long)lba, (unsigned long)cnt, ok);
        if (ok) {
            s_phase = PH_DATAIN;
            raise_cd_irq(IRQ_DATA_IN);
        } else {
            enter_status(0x02);
        }
        break;
    }

    case 0xD8: /* SET AUDIO PLAYBACK START (CD-DA) — audio not yet implemented */
    case 0xD9: /* SET AUDIO PLAYBACK END */
    case 0xDA: /* PAUSE */
    case 0xDB: /* ... */
    case 0xDD: /* READ SUBCHANNEL */
    case 0xDE: /* GET DIR INFO (TOC) — minimal: report good for now */
        enter_status(0x00);
        break;

    default:
        enter_status(0x00); /* be permissive in iteration 1 */
        break;
    }
}

uint8_t pce_scsi_read(uint8_t reg)
{
    switch (reg & 0x0F) {
    case 0x00: { /* bus status */
        uint8_t st = 0;
        switch (s_phase) {
        case PH_BUSFREE: st = 0; break;
        case PH_COMMAND: st = ST_BSY | ST_REQ | ST_CD; break;            /* out */
        case PH_DATAIN:  st = ST_BSY | ST_REQ | ST_IO; break;            /* in, data */
        case PH_STATUS:  st = ST_BSY | ST_REQ | ST_CD | ST_IO; break;    /* in, status */
        case PH_MSGIN:   st = ST_BSY | ST_REQ | ST_CD | ST_IO | ST_MSG; break;
        }
        return st;
    }
    case 0x01: { /* data bus in */
        if (s_phase == PH_DATAIN) {
            uint8_t b = s_sector[16 + s_data_ptr];
            if (++s_data_ptr >= s_data_len) {
                if (!load_next_sector()) {
                    /* all requested sectors sent -> status phase */
                    enter_status(0x00);
                }
            }
            return b;
        }
        if (s_phase == PH_STATUS) {
            uint8_t b = s_status;
            s_phase = PH_MSGIN;
            return b;
        }
        if (s_phase == PH_MSGIN) {
            uint8_t b = s_message;
            s_phase = PH_BUSFREE;     /* command complete */
            raise_cd_irq(IRQ_STATUS);
            return b;
        }
        return 0;
    }
    case 0x02: return s_irq_enable;
    case 0x03: return s_irq_status | 0x10; /* BRAM unlocked bit kept set */
    case 0x04: return 0;
    case 0x07: return 0x00;                /* BRAM not present */
    default:   return 0;
    }
}

void pce_scsi_write(uint8_t reg, uint8_t val)
{
    switch (reg & 0x0F) {
    case 0x00: /* bus control: a write with no select just idles */
        break;
    case 0x01: /* data bus out (command byte during COMMAND phase) */
        if (s_phase == PH_BUSFREE) {
            /* first byte selects the device + starts a command */
            s_phase = PH_COMMAND;
            s_cmd_idx = 0;
            s_cmd_len = 6;            /* PCE uses 6-byte CDBs */
        }
        if (s_phase == PH_COMMAND && s_cmd_idx < (int)sizeof(s_cmd)) {
            s_cmd[s_cmd_idx++] = val;
            if (s_cmd_idx >= s_cmd_len)
                execute_command();
        }
        break;
    case 0x02: /* IRQ enable */
        s_irq_enable = val;
        if (!(val & IRQ_DATA_IN) && !(val & IRQ_STATUS))
            CPU_PCE.irq_lines &= ~INT_IRQ2;
        break;
    case 0x03: /* ack / clear */
        s_irq_status &= ~val;
        CPU_PCE.irq_lines &= ~INT_IRQ2;
        break;
    case 0x04: /* reset */
        if (val & 0x02)
            pce_scsi_reset();
        break;
    default:
        break;
    }
}

void pce_scsi_run(void)
{
    /* IRQ assertion is event-driven in this iteration; nothing periodic yet. */
}
