/* PC Engine CD-ROM2 SCSI target. See pce_scsi.h.
 *
 * Register + handshake semantics ported from Mednafen pce_fast (pcecd.c /
 * pcecd_drive.c): $1800w = SEL pulse (selects drive -> COMMAND phase); command
 * bytes via $1801 DB-out + $1802 bit7 ACK; $1800r bit7..3 = BSY/REQ/MSG/CD/IO;
 * $1802 = IRQ-enable, $1803 = IRQ-status, IRQ2 = port2 & port3 & 0x7C with
 * 0x40=DATA-READY, 0x20=DATA-DONE. Commands: TEST UNIT READY, GET DIR INFO
 * (0xDE, the TOC the System Card needs to find the data track), READ(6); audio
 * commands ack OK (ADPCM/CD-DA not yet). Sectors come from pce_cd. */
#include "pce_scsi.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "h6280.h"          /* CPU_PCE, INT_IRQ2 */

/* ---- diagnostics: append the command stream to /pcecd_diag.txt (delete it
 *      before a clean test; capped so it can't flood). ---- */
#define PCECD_DIAG 1
#if PCECD_DIAG
static int s_diag_lines;
static void diag(const char *fmt, ...)
{
    if (s_diag_lines > 300) return;
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

#define STATUS_GOOD            0x00
#define STATUS_CHECK_CONDITION 0x02
#define IRQ_DATA_DONE          0x20   /* $1803 */
#define IRQ_DATA_READY         0x40
#define IRQ_MASK               0x7C   /* 0x4|0x8|0x10|0x20|0x40 */

enum { PH_BUSFREE, PH_COMMAND, PH_DATAIN, PH_STATUS, PH_MSGIN };

static const pce_cd_toc_t *s_toc;
static bool     s_present;

static int      s_phase;
static bool     s_bsy, s_req, s_msg, s_cd, s_io, s_ack;
static uint8_t  s_db;          /* SCSI data bus */
static uint8_t  s_cmd[16];
static int      s_cmd_idx;
static uint8_t  s_message;

static uint8_t  s_din[2048];   /* current data-in buffer (command response or sector) */
static int      s_din_pos, s_din_len;
static bool     s_reading;     /* READ(6) sector-stream mode */
static uint32_t s_read_lba, s_read_remain;

static uint8_t  s_port2, s_port3;   /* $1802 IRQ-enable, $1803 IRQ-status */

static void update_irq(void)
{
    if (s_port2 & s_port3 & IRQ_MASK)
        CPU_PCE.irq_lines |= INT_IRQ2;
    else
        CPU_PCE.irq_lines &= ~INT_IRQ2;
}

void pce_scsi_set_disc(const pce_cd_toc_t *toc, bool present)
{
    s_toc = toc;
    s_present = present && toc && toc->num_tracks > 0;
    s_diag_lines = 0;   /* fresh run */
    diag("=== BUILD it5 ===\n");
    diag("MOUNT present=%d tracks=%d total_lba=%lu\n", s_present,
         toc ? toc->num_tracks : -1, (unsigned long)(toc ? toc->total_lba : 0));
    pce_scsi_reset();
}

void pce_scsi_reset(void)
{
    s_phase = PH_BUSFREE;
    s_bsy = s_req = s_msg = s_cd = s_io = s_ack = 0;
    s_db = 0;
    s_cmd_idx = 0;
    s_message = 0;
    s_din_pos = s_din_len = 0;
    s_reading = false;
    s_read_remain = 0;
    s_port2 = s_port3 = 0;
    CPU_PCE.irq_lines &= ~INT_IRQ2;
}

static void change_phase(int ph)
{
    s_phase = ph;
    switch (ph) {
    case PH_BUSFREE: s_bsy = s_req = s_msg = s_cd = s_io = 0; s_port3 |= IRQ_DATA_DONE; diag("  DONE\n"); break;
    case PH_COMMAND: s_bsy = 1; s_cd = 1; s_io = 0; s_msg = 0; s_req = 1; s_cmd_idx = 0; break;
    case PH_DATAIN:  s_bsy = 1; s_io = 1; s_cd = 0; s_msg = 0; s_req = 0; s_port3 |= IRQ_DATA_READY; break;
    case PH_STATUS:  s_bsy = 1; s_io = 1; s_cd = 1; s_msg = 0; s_req = 1; break;
    case PH_MSGIN:   s_bsy = 1; s_io = 1; s_cd = 1; s_msg = 1; s_req = 1; s_db = s_message; break;
    }
    update_irq();
}

static void send_status(uint8_t status, uint8_t message)
{
    s_message = message;
    s_db = (status == STATUS_GOOD) ? 0x00 : 0x01;
    change_phase(PH_STATUS);
}

/* Pull the next user sector (MODE1 payload at raw offset 16) into s_din. */
static bool load_sector(void)
{
    static uint8_t raw[PCE_CD_SECTOR_RAW];
    if (!s_present || s_read_remain == 0) return false;
    if (!pce_cd_read_sector(s_toc, s_read_lba, raw)) return false;
    memcpy(s_din, raw + 16, 2048);
    s_din_pos = 0; s_din_len = 2048;
    s_read_lba++; s_read_remain--;
    return true;
}

static int din_get(void)
{
    if (s_din_pos >= s_din_len) {
        if (s_reading && s_read_remain > 0) { if (!load_sector()) return -1; }
        else return -1;
    }
    return s_din[s_din_pos++];
}

/* Present the next data-in byte (assert REQ), or finish the transfer. */
static void feed_din(void)
{
    int b = din_get();
    if (b < 0) { s_reading = false; send_status(STATUS_GOOD, 0); }
    else       { s_db = (uint8_t)b; s_req = 1; }
}

static void do_data_in(const uint8_t *buf, uint32_t len)
{
    if (len > sizeof(s_din)) len = sizeof(s_din);
    memcpy(s_din, buf, len);
    s_din_pos = 0; s_din_len = (int)len;
    s_reading = false;
    diag("  DATAIN len=%lu\n", (unsigned long)len);
    change_phase(PH_DATAIN);
    feed_din();
}

static uint8_t u8_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint8_t bcd_to_u8(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

static void lba_to_amsf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
    uint32_t t = lba + PCE_CD_LEADIN_LBA;   /* absolute MSF */
    *m = (uint8_t)(t / (75 * 60));
    *s = (uint8_t)((t / 75) % 60);
    *f = (uint8_t)(t % 75);
}

/* 0xDE GET DIR INFO — the TOC query the System Card uses to locate tracks. */
static void get_dir_info(void)
{
    uint8_t out[8] = {0};
    uint32_t len = 0;
    switch (s_cmd[1]) {
    case 0x0:
        out[0] = u8_to_bcd(s_toc->tracks[0].number);                 /* first track */
        out[1] = u8_to_bcd(s_toc->tracks[s_toc->num_tracks - 1].number); /* last */
        len = 2;
        break;
    case 0x1: { /* lead-out MSF */
        uint8_t m, s, f; lba_to_amsf(s_toc->total_lba, &m, &s, &f);
        out[0] = u8_to_bcd(m); out[1] = u8_to_bcd(s); out[2] = u8_to_bcd(f);
        len = 3;
        break;
    }
    case 0x2: { /* per-track start MSF + control */
        uint32_t lba = s_toc->total_lba; uint8_t ctrl = 0x04;
        if (s_cmd[2] != 0xAA) {
            int t = bcd_to_u8(s_cmd[2]); if (!t) t = 1;
            for (int i = 0; i < s_toc->num_tracks; i++)
                if (s_toc->tracks[i].number == t) {
                    lba = s_toc->tracks[i].start_lba;
                    ctrl = (s_toc->tracks[i].type == PCE_TRACK_DATA) ? 0x04 : 0x00;
                    break;
                }
        }
        uint8_t m, s, f; lba_to_amsf(lba, &m, &s, &f);
        out[0] = u8_to_bcd(m); out[1] = u8_to_bcd(s); out[2] = u8_to_bcd(f); out[3] = ctrl;
        len = 4;
        break;
    }
    }
    do_data_in(out, len);
}

static void execute_command(void)
{
    uint8_t op = s_cmd[0];
    diag("CMD %02x %02x %02x %02x %02x %02x p2=%02x\n",
         s_cmd[0], s_cmd[1], s_cmd[2], s_cmd[3], s_cmd[4], s_cmd[5], s_port2);

    if (!s_present && op != 0x03) { send_status(STATUS_CHECK_CONDITION, 0); return; }

    switch (op) {
    case 0x00: /* TEST UNIT READY */
        send_status(STATUS_GOOD, 0);
        break;
    case 0x03: { /* REQUEST SENSE (minimal) */
        uint8_t sense[18] = {0}; sense[0] = 0x70;
        uint32_t n = s_cmd[4] ? s_cmd[4] : 14; if (n > sizeof(sense)) n = sizeof(sense);
        do_data_in(sense, n);
        break;
    }
    case 0x08: { /* READ(6) */
        uint32_t lba = ((uint32_t)(s_cmd[1] & 0x1F) << 16) | ((uint32_t)s_cmd[2] << 8) | s_cmd[3];
        uint32_t cnt = s_cmd[4] ? s_cmd[4] : 1;
        s_read_lba = lba; s_read_remain = cnt; s_reading = true;
        s_din_pos = s_din_len = 0;
        diag("  READ lba=%lu cnt=%lu\n", (unsigned long)lba, (unsigned long)cnt);
        change_phase(PH_DATAIN);
        feed_din();
        if (s_phase == PH_STATUS) diag("  READ failed (no sector)\n");
        break;
    }
    case 0xDE: /* GET DIR INFO (TOC) */
        get_dir_info();
        break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDD: /* audio: ack OK for now */
        send_status(STATUS_GOOD, 0);
        break;
    default:
        send_status(STATUS_GOOD, 0);
        break;
    }
}

/* ACK rising edge: the current REQ byte is transferred. */
static void ack_assert(void)
{
    if (!s_req) return;
    switch (s_phase) {
    case PH_COMMAND: if (s_cmd_idx < (int)sizeof(s_cmd)) s_cmd[s_cmd_idx++] = s_db; s_req = 0; break;
    case PH_DATAIN:  s_req = 0; break;   /* ACK toggles REQ; the byte advanced on $1801 read */
    case PH_STATUS:  s_req = 0; break;
    case PH_MSGIN:   s_req = 0; break;
    }
}

/* ACK falling edge: advance to the next byte / phase. */
static void ack_deassert(void)
{
    switch (s_phase) {
    case PH_COMMAND: if (s_cmd_idx >= 6) execute_command(); else s_req = 1; break;
    case PH_DATAIN:  s_req = 1; break;   /* REQ back up for the next $1801 read (no advance here) */
    case PH_STATUS:  change_phase(PH_MSGIN); break;
    case PH_MSGIN:   change_phase(PH_BUSFREE); break;
    }
}

uint8_t pce_scsi_read(uint8_t reg)
{
    switch (reg & 0x0F) {
    case 0x00:
        return (uint8_t)((s_bsy ? 0x80 : 0) | (s_req ? 0x40 : 0) | (s_msg ? 0x20 : 0)
                       | (s_cd ? 0x10 : 0) | (s_io ? 0x08 : 0));
    case 0x01:
        if (s_phase == PH_DATAIN) {
            /* Data-in is hardware auto-acked: reading the data port pulls the
             * current byte and presents the next (or ends the transfer). */
            uint8_t b = s_db;
            feed_din();
            return b;
        }
        return s_db;
    case 0x02: return s_port2;
    case 0x03: return s_port3;
    case 0x04: return 0;
    default:   return 0;
    }
}

void pce_scsi_write(uint8_t reg, uint8_t val)
{
    switch (reg & 0x0F) {
    case 0x00: /* SEL pulse: select the drive -> COMMAND phase */
        if (!s_bsy) change_phase(PH_COMMAND);
        s_port3 &= ~(IRQ_DATA_DONE | IRQ_DATA_READY);
        update_irq();
        break;
    case 0x01: /* data bus out */
        s_db = val;
        break;
    case 0x02: { /* IRQ-enable + ACK (bit7) */
        bool nack = (val & 0x80) != 0;
        s_port2 = val;
        if (nack && !s_ack)      ack_assert();
        else if (!nack && s_ack) ack_deassert();
        s_ack = nack;
        update_irq();
        break;
    }
    case 0x04: /* reset */
        if (val & 0x02) pce_scsi_reset();
        break;
    default:
        break;
    }
}

void pce_scsi_run(void) { }
