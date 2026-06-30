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
#include "pce_adpcm.h"      /* $1808-$180E ADPCM voice */

/* The pce-go submodule's h6280.c has a gated per-instruction diag hook
 * (`if (g_pcecd_trace) pce_scsi_pc_tick(pc)`). Provide WEAK definitions here so
 * the device firmware links (g_pcecd_trace stays 0 → the hook is a no-op); the
 * PC host harness defines strong versions in main.c that override these. */
__attribute__((weak)) int  g_pcecd_trace = 0;
__attribute__((weak)) void pce_scsi_pc_tick(uint16_t pc) { (void)pc; }

/* ---- diagnostics: append the command stream to /pcecd_diag.txt (delete it
 *      before a clean test; capped so it can't flood). ---- */
#define PCECD_DIAG 1
#ifdef LINUX_EMU
  #define PCECD_DIAG_FILE "pcecd_diag.txt"   /* host harness: writable cwd */
#else
  #define PCECD_DIAG_FILE "/pcecd_diag.txt"  /* device: SD root */
#endif
#if PCECD_DIAG
static int s_diag_lines;
static void diag(const char *fmt, ...)
{
    if (s_diag_lines > 4000) return;
    s_diag_lines++;
    FILE *f = fopen(PCECD_DIAG_FILE, "a");
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
static bool     s_bulk;        /* data-in is a bulk READ (auto-ack on $1801 read) vs TOC (manual ACK) */
static uint32_t s_read_lba, s_read_remain;

static uint8_t  s_port2, s_port3;   /* $1802 IRQ-enable, $1803 IRQ-status */
static uint8_t  s_adpcm_ctrl;       /* $180B ADPCM DMA control latch */

/* SCSI CDB length by opcode high nibble (Mednafen pce_fast). The NEC audio +
 * TOC commands (0xDn/0xEn) are 10 bytes — we must pull all 10 so the SAPSP/SAPEP
 * addressing mode byte cdb[9] is available for CD-DA. READ(6)/TEST = 6. */
static const uint8_t RequiredCDBLen[16] = {
    6, 6, 10, 10, 10, 10, 10, 10, 10, 10, 12, 12, 10, 10, 10, 10,
};

/* ---- CD-DA (Red Book audio / BGM) ---- */
static bool     s_cdda_play;            /* currently streaming audio */
static uint32_t s_cdda_lba, s_cdda_end, s_cdda_start; /* current/end/start sector */
static uint8_t  s_cdda_sec[PCE_CD_SECTOR_RAW];
static int      s_cdda_pos;             /* byte offset within s_cdda_sec (>= raw means reload) */
static int      s_cdda_mode;            /* SAPEP play mode: 1=loop, 3=normal */
static int      s_trace;            /* trace register accesses during a bulk READ */
static int      s_atrace;           /* trace ADPCM/idle-loop polls ($180A-F, $1803) */

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
    diag("=== BUILD scd-adpcm-fix ===\n");
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
    s_bulk = false;
    s_read_remain = 0;
    s_port2 = s_port3 = 0;
    s_adpcm_ctrl = 0;
    s_cdda_play = false;
    pce_adpcm_reset();
    CPU_PCE.irq_lines &= ~INT_IRQ2;
}

static void change_phase(int ph)
{
    s_phase = ph;
    switch (ph) {
    /* DATA_READY (0x40) marks the DATAIN transfer in progress; clear it once the
     * transfer ends (STATUS / BUSFREE) or $1803 sticks at 0x60 and the System Card
     * polls it forever (observed on device). DATA_DONE (0x20) signals completion. */
    case PH_BUSFREE: s_bsy = s_req = s_msg = s_cd = s_io = 0; s_port3 = (s_port3 & ~IRQ_DATA_READY) | IRQ_DATA_DONE; diag("  DONE\n"); break;
    case PH_COMMAND: s_bsy = 1; s_cd = 1; s_io = 0; s_msg = 0; s_req = 1; s_cmd_idx = 0; break;
    case PH_DATAIN:  s_bsy = 1; s_io = 1; s_cd = 0; s_msg = 0; s_req = 0; s_port3 |= IRQ_DATA_READY; break;
    case PH_STATUS:  s_bsy = 1; s_io = 1; s_cd = 1; s_msg = 0; s_req = 1; s_port3 &= ~IRQ_DATA_READY; break;
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

/* ADPCM ($180A-$180D). The System Card loads ADPCM (voice) data straight from CD
 * by issuing a READ(6) then enabling SCSI->ADPCM DMA via $180B bit1; it then polls
 * $180C (ADPCM busy) and $1803 (DATA_DONE) for completion. We don't run a real
 * ADPCM engine yet, so when DMA is enabled during a bulk READ we drain the whole
 * transfer at once (consuming every sector) and signal completion, so the BIOS
 * loop exits and the game proceeds instead of hanging/rebooting. */
static void adpcm_dma_drain(void)
{
    if (!s_reading) return;
    extern void wdog_refresh(void);
    int b;
    unsigned long n = 0;
    while ((b = din_get()) >= 0) {
        pce_adpcm_dma_byte((uint8_t)b);                 /* CD -> ADPCM RAM */
        if ((++n & 0x7FF) == 0) wdog_refresh();         /* feed wdog on big FMV streams */
    }
    s_reading = false;
    diag("  ADPCM drain %lu B\n", n);
    /* Completion needs BOTH, in this order, for the System Card's ADPCM-load path:
     *  - $1803 DATA_DONE set NOW (the transfer-complete IRQ flag the f3d0 loop polls
     *    BEFORE the status handshake), and
     *  - the bus presented in STATUS phase ($1800 & $F8 == $D8) so the following
     *    $E9C5 status-wait can read the result byte and run the normal handshake. */
    send_status(STATUS_GOOD, 0);
    s_port3 |= IRQ_DATA_DONE;
}

static void do_data_in(const uint8_t *buf, uint32_t len)
{
    if (len > sizeof(s_din)) len = sizeof(s_din);
    memcpy(s_din, buf, len);
    s_din_pos = 0; s_din_len = (int)len;
    s_reading = false; s_bulk = false;
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

/* Decode a SAPSP/SAPEP audio position. cmd[9]&0xC0 selects the mode (Mednafen):
 * 0x00=LBA(cmd[3..5]), 0x40=MSF BCD(cmd[2..4]), 0x80=track number BCD(cmd[2]). */
static uint32_t cdda_decode_pos(const uint8_t *cmd)
{
    switch (cmd[9] & 0xC0) {
    case 0x00:
        return ((uint32_t)cmd[3] << 16) | ((uint32_t)cmd[4] << 8) | cmd[5];
    case 0x40: {
        uint32_t lba = (bcd_to_u8(cmd[2]) * 60u + bcd_to_u8(cmd[3])) * 75u + bcd_to_u8(cmd[4]);
        return (lba >= PCE_CD_LEADIN_LBA) ? lba - PCE_CD_LEADIN_LBA : 0;
    }
    default: {
        int t = bcd_to_u8(cmd[2]); if (!t) t = 1;
        if (s_toc) for (int i = 0; i < s_toc->num_tracks; i++)
            if (s_toc->tracks[i].number == t) return s_toc->tracks[i].start_lba;
        return s_toc ? s_toc->total_lba : 0;
    }
    }
}

/* Fill `frames` stereo int16 samples at half the CD rate (44100/2 = 22050) by
 * decimating. Returns frames produced (0 = not playing). Reads raw 2352B audio
 * sectors on demand. */
int pce_scsi_cdda_fill(int16_t *out, int frames)
{
    if (!s_cdda_play || !s_present) return 0;
    for (int i = 0; i < frames; i++) {
        if (s_cdda_pos + 8 > PCE_CD_SECTOR_RAW) {
            if (s_cdda_lba >= s_cdda_end) {
                if (s_cdda_mode == 1) {            /* LOOP: restart at the start sector */
                    s_cdda_lba = s_cdda_start;
                } else {                           /* NORMAL: stop, pad the rest with silence */
                    s_cdda_play = false;
                    for (; i < frames; i++) { out[i * 2] = 0; out[i * 2 + 1] = 0; }
                    return frames;
                }
            }
            if (!pce_cd_read_sector(s_toc, s_cdda_lba, s_cdda_sec)) { s_cdda_play = false; return i; }
            s_cdda_lba++;
            s_cdda_pos = 0;
        }
        /* Average the two 44.1k frames into one 22.05k frame — a cheap 2-tap
         * box low-pass that removes the harsh aliasing of plain drop-decimation. */
        int16_t l0 = (int16_t)(s_cdda_sec[s_cdda_pos]     | (s_cdda_sec[s_cdda_pos + 1] << 8));
        int16_t r0 = (int16_t)(s_cdda_sec[s_cdda_pos + 2] | (s_cdda_sec[s_cdda_pos + 3] << 8));
        int16_t l1 = (int16_t)(s_cdda_sec[s_cdda_pos + 4] | (s_cdda_sec[s_cdda_pos + 5] << 8));
        int16_t r1 = (int16_t)(s_cdda_sec[s_cdda_pos + 6] | (s_cdda_sec[s_cdda_pos + 7] << 8));
        out[i * 2]     = (int16_t)((l0 + l1) >> 1);
        out[i * 2 + 1] = (int16_t)((r0 + r1) >> 1);
        s_cdda_pos += 8;                            /* consume 2 CD frames, emit 1 (decimate /2) */
    }
    return frames;
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
        s_read_lba = lba; s_read_remain = cnt; s_reading = true; s_bulk = true;
        s_din_pos = s_din_len = 0;
        s_trace = 0;   /* trace the System Card's register pattern for this READ */
        diag("  READ lba=%lu cnt=%lu\n", (unsigned long)lba, (unsigned long)cnt);
        change_phase(PH_DATAIN);
        feed_din();
        if (s_phase == PH_STATUS) diag("  READ failed (no sector)\n");
        break;
    }
    case 0xDE: /* GET DIR INFO (TOC) */
        get_dir_info();
        break;
    case 0xD8: /* SAPSP — set audio playback start position (+ play if cmd[1]) */
        s_cdda_start = s_cdda_lba = cdda_decode_pos(s_cmd);
        s_cdda_end   = s_toc ? s_toc->total_lba : 0;
        s_cdda_pos   = PCE_CD_SECTOR_RAW;          /* force a fresh sector load */
        s_cdda_mode  = 3;
        s_cdda_play  = (s_cmd[1] != 0);
        diag("  CDDA SAPSP lba=%lu play=%d\n", (unsigned long)s_cdda_lba, s_cdda_play);
        send_status(STATUS_GOOD, 0);
        break;
    case 0xD9: /* SAPEP — set end position + play mode (1=loop 2=int 3=normal 0=stop) */
        s_cdda_end  = cdda_decode_pos(s_cmd);
        s_cdda_mode = s_cmd[1];
        s_cdda_play = (s_cmd[1] != 0);
        s_cdda_pos  = PCE_CD_SECTOR_RAW;
        diag("  CDDA SAPEP end=%lu mode=%d\n", (unsigned long)s_cdda_end, s_cmd[1]);
        send_status(STATUS_GOOD, 0);
        break;
    case 0xDA: /* PAUSE */
        s_cdda_play = false;
        send_status(STATUS_GOOD, 0);
        break;
    case 0xDD: /* READ SUBCHANNEL Q — minimal ack */
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
    case PH_COMMAND: if (s_cmd_idx >= RequiredCDBLen[s_cmd[0] >> 4]) execute_command(); else s_req = 1; break;
    case PH_DATAIN:  feed_din(); break;  /* advance on ACK (TOC + manual-ack READs); $1808 reads advance separately */
    case PH_STATUS:  change_phase(PH_MSGIN); break;
    case PH_MSGIN:   change_phase(PH_BUSFREE); break;
    }
}

uint8_t pce_scsi_read(uint8_t reg)
{
    if (s_bulk && s_phase == PH_DATAIN && s_trace < 12) {
        diag("R%x db=%02x\n", reg & 0xf, s_db);
        s_trace++;
    }
    if (s_phase == PH_BUSFREE && s_atrace < 130) {
        diag("Ir%x db=%02x p3=%02x p2=%02x\n", reg & 0xf, s_db, s_port3, s_port2);
        s_atrace++;
    }
    switch (reg & 0x0F) {
    case 0x00:
        return (uint8_t)((s_bsy ? 0x80 : 0) | (s_req ? 0x40 : 0) | (s_msg ? 0x20 : 0)
                       | (s_cd ? 0x10 : 0) | (s_io ? 0x08 : 0));
    case 0x01: return s_db;   /* command/TOC/status byte; advances on ACK */
    case 0x02: return s_port2;
    case 0x03: return s_port3;
    case 0x04: return 0;
    case 0x08: {
        /* $1808 = SCSI auto-increment data read. The System Card pulls BULK READ
         * data through here: return the current byte and, in data-in, auto-ack
         * to advance to the next (mirrors Mednafen read_1808). */
        uint8_t b = s_db;
        if (s_phase == PH_DATAIN && s_req)
            feed_din();
        return b;
    }
    case 0x0A: return pce_adpcm_read(0x0A);   /* ADPCM RAM data */
    case 0x0B: return s_adpcm_ctrl;
    case 0x0C: return pce_adpcm_read(0x0C);   /* ADPCM status (end/playing) */
    default:   return 0;
    }
}

void pce_scsi_write(uint8_t reg, uint8_t val)
{
    if (s_bulk && s_phase == PH_DATAIN && s_trace < 12) {
        diag("W%x=%02x\n", reg & 0xf, val);
        s_trace++;
    }
    if (s_phase == PH_BUSFREE && s_atrace < 130) {
        diag("Iw%x=%02x\n", reg & 0xf, val);
        s_atrace++;
    }
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
    case 0x08: case 0x09: case 0x0A: case 0x0D: case 0x0E:
        pce_adpcm_write(reg, val);   /* ADPCM addr/data/control/rate */
        break;
    case 0x0B: /* ADPCM DMA control: bit1 = enable SCSI->ADPCM auto-transfer */
        s_adpcm_ctrl = val;
        if (val & 0x02) adpcm_dma_drain();
        break;
    case 0x04: /* reset */
        if (val & 0x02) pce_scsi_reset();
        break;
    default:
        break;
    }
}

void pce_scsi_run(void) { }
