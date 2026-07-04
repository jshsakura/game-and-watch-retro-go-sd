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
__attribute__((weak)) int  g_pce_kill_timer = 0;
__attribute__((weak)) void pce_scsi_pc_tick(uint16_t pc) { (void)pc; }

/* ---- diagnostics: append the command stream to pcecd_diag.txt. HOST ONLY: on-device the
 * per-command fopen, repeated thousands of times during the System-Card poll loop while the
 * .bin is held open, crashed to the blue FATAL screen. Host harness has no file limit. ---- */
#ifdef LINUX_EMU
  #define PCECD_DIAG 1
  #define PCECD_DIAG_FILE "pcecd_diag.txt"   /* host harness: writable cwd */
  #define PCECD_DIAG_MAX  4000
#else
  /* Device: RE-ENABLED. fopen routes to FatFs (syscalls.c MAX_OPEN_FILES=10,
   * FF_FS_LOCK=0), NOT the littlefs 1-file limit — logging alongside the open .bin
   * is safe. The old "diag caused the FATAL" was the wrong premise (same as C64);
   * the real FATAL risk is the per-command fopen+f_sync repeated thousands of times
   * in the System-Card poll loop, so cap tight at 400 lines (the per-category caps
   * s_atrace<130 / s_trace<12 already bound the HIGH-FREQUENCY bulk (that flood — not the
   * global line count — was the FATAL risk, and it stays capped regardless), so this global
   * cap only buys headroom for the low-rate events we actually want (CD-DA / ADPCM start,
   * READ fails). 800 lines ~= 32KB, nothing on SD. Delete /pcecd_diag.txt first. */
  /* OFF for the clean release: PCE-CD load/resume/Dynastic verification is
   * complete (2026-07-04). Flip back to 1 if a new PCE-CD issue needs traces. */
  #define PCECD_DIAG 0
  #define PCECD_DIAG_FILE "/pcecd_diag.txt"
  #define PCECD_DIAG_MAX  800
#endif
#if PCECD_DIAG
static int s_diag_lines;
static void diag(const char *fmt, ...)
{
    if (s_diag_lines > PCECD_DIAG_MAX) return;
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
static bool     s_cdda_paused;          /* PAUSE(0xDA)/seek state: position held, not streaming */
static uint32_t s_head_lba;             /* laser head: last DATA sector read (SUBQ reports it
                                           when CD-DA is not playing — real drives do) */
static uint32_t s_cdda_lba, s_cdda_end, s_cdda_start; /* current/end/start sector */
/* CD-DA is streamed through a small sector FIFO topped up a LITTLE every audio
 * frame, instead of a batch that read many sectors at once when it drained. The
 * batch refill blocked pce_pcm_submit (main loop) with a ~9KB fread every ~3
 * frames = a periodic hitch ("dragging" audio). A frame only consumes ~1.25
 * sectors, so reading at most PCE_CDDA_TOPUP sectors per call keeps the SD cost
 * small and EVEN, while PCE_CDDA_RING sectors of depth absorb read jitter. */
#define PCE_CDDA_RING  6                /* FIFO depth in sectors (~5 frames of slack) */
#define PCE_CDDA_TOPUP 3                /* max sectors read per fill call (> the ~1.25/frame
                                          consumption so it keeps ahead, but bounded = no burst) */
static uint8_t  s_cdda_sec[PCE_CD_SECTOR_RAW * PCE_CDDA_RING];
static int      s_cdda_pos;             /* read cursor (bytes) */
static int      s_cdda_have;            /* valid bytes (0 = empty FIFO) */
static int      s_cdda_mode;            /* SAPEP play mode: 1=loop, 3=normal */
static int      s_trace;            /* trace register accesses during a bulk READ */
static int      s_atrace;           /* trace ADPCM/idle-loop polls ($180A-F, $1803) */

/* $1803 bit 0x08 = ADPCM sample END — a LEVEL flag mirrored straight from the
 * ADPCM engine (cleared when a new play latches length, engine side). Games
 * enable it via $1802 bit3 and SLEEP until the end IRQ to queue the next music
 * segment; without this wire a state-load that resumed mid-segment froze ~2s
 * later, exactly when the restored segment ran out (Cotton). */
#define IRQ_ADPCM_END 0x08

static uint8_t effective_port3(void)
{
    return (uint8_t)(s_port3 | (pce_adpcm_end_flag() ? IRQ_ADPCM_END : 0));
}

static void update_irq(void)
{
    uint8_t ep3 = effective_port3();
    if (s_port2 & ep3 & IRQ_MASK) {
        /* One-shot breadcrumbs: prove on-device that the ADPCM-END wire exists
         * and the IRQ2 line actually rises after a state load. */
        { static bool logged_end; if (!logged_end && (s_port2 & ep3 & IRQ_ADPCM_END)) {
            logged_end = true; diag("  ADPCM END IRQ asserted\n"); } }
        CPU_PCE.irq_lines |= INT_IRQ2;
    } else {
        CPU_PCE.irq_lines &= ~INT_IRQ2;
    }
}

void pce_scsi_set_disc(const pce_cd_toc_t *toc, bool present)
{
    s_toc = toc;
    s_present = present && toc && toc->num_tracks > 0;
#if PCECD_DIAG
    s_diag_lines = 0;   /* fresh run */
#endif
    diag("=== BUILD adpcmshift-1 ===\n");
    diag("MOUNT present=%d tracks=%d total_lba=%lu\n", s_present,
         toc ? toc->num_tracks : -1, (unsigned long)(toc ? toc->total_lba : 0));
#ifndef LINUX_EMU
    /* Prove the PCE-CD auto-OC engaged (280 = stock/OSPI1-guarded, ~353 = lvl2). */
    { extern uint32_t HAL_RCC_GetSysClockFreq(void);
      diag("clock=%lu MHz (auto-OC lvl2 requested)\n",
           (unsigned long)(HAL_RCC_GetSysClockFreq() / 1000000)); }
#endif
    /* Dump every track's computed start_lba — the harness showed device reads land 294
     * sectors below host for identical data, i.e. the per-track LBA computation diverges
     * on device. Compare this dump host-vs-device to find where the offset creeps in. */
    if (toc) for (int i = 0; i < toc->num_tracks; i++)
        diag("TOC t%02d type=%d start=%lu off=%lu ss=%d %s\n",
             toc->tracks[i].number, toc->tracks[i].type,
             (unsigned long)toc->tracks[i].start_lba, (unsigned long)toc->tracks[i].file_offset,
             toc->tracks[i].sector_size, toc->tracks[i].bin_path);
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
    s_cdda_paused = false;
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
    s_head_lba = s_read_lba;
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
static uint32_t s_read_served;   /* bytes actually handed to the game this READ */
static void feed_din(void)
{
    int b = din_get();
    if (b < 0) {
        s_reading = false;
        diag("  READ served %lu B\n", (unsigned long)s_read_served);
        send_status(STATUS_GOOD, 0);
    }
    else       { s_db = (uint8_t)b; s_req = 1; s_read_served++; }
}

/* ADPCM ($180A-$180D). The System Card loads ADPCM (voice) data straight from CD
 * by issuing a READ(6) then enabling SCSI->ADPCM DMA via $180B bit1; it then polls
 * $180C (ADPCM busy) and $1803 (DATA_DONE) for completion. We don't run a real
 * DMA engine, so the transfer is PUMPED from the main loop (pce_scsi_run), up to
 * 4 sectors (8KB) per frame: the old drain-everything-now path pulled a 64KB FMV
 * load through SD in ONE frame = a 30-55ms single-shot stall (the pacer's worst
 * enemy). Chunked, the same load spreads over ~8 frames — still far faster than
 * the ~430ms a real 1x drive needed, so the BIOS poll loop ($1802/$1803) simply
 * sees "in progress" for a few frames, exactly as on hardware. */
static bool     s_adpcm_dma_active;
static uint32_t s_adpcm_dma_total;

static void adpcm_dma_pump(void)
{
    if (!s_adpcm_dma_active) return;
    int budget = 4 * 2048;                              /* <=4 sectors per frame */
    int b = -1;
    while (budget > 0 && (b = din_get()) >= 0) {
        pce_adpcm_dma_byte((uint8_t)b);                 /* CD -> ADPCM RAM */
        s_adpcm_dma_total++;
        budget--;
    }
    if (b >= 0) return;                                 /* more next frame */
    s_adpcm_dma_active = false;
    s_reading = false;
    diag("  ADPCM drain %lu B\n", (unsigned long)s_adpcm_dma_total);
    /* Completion needs BOTH, in this order, for the System Card's ADPCM-load path:
     *  - $1803 DATA_DONE set NOW (the transfer-complete IRQ flag the f3d0 loop polls
     *    BEFORE the status handshake), and
     *  - the bus presented in STATUS phase ($1800 & $F8 == $D8) so the following
     *    $E9C5 status-wait can read the result byte and run the normal handshake. */
    send_status(STATUS_GOOD, 0);
    s_port3 |= IRQ_DATA_DONE;
    update_irq();   /* refresh IRQ2 with the just-set DATA_DONE: send_status/change_phase
                       ran update_irq() BEFORE we OR'd DATA_DONE in, so without this the
                       ADPCM-load-complete IRQ never asserts and the System Card poll-loops
                       $1802/$1803 forever right after the opening (Dracula X "opening then
                       stops"). Every other DATA_DONE set refreshes the line via change_phase. */
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

/* Top up the FIFO tail with up to `want` raw audio sectors, batched: one fread
 * per contiguous run instead of one per sector (each fseek+fread pair costs an
 * SD command round-trip; batching flattens the periodic multi-sector frame
 * spike the pacer is sensitive to). Handles loop wrap and end-of-stream; track
 * boundaries just split the batch (pce_cd_read_sectors_audio clamps at the
 * track end and the loop continues into the next track). */
/* One bounded batched read into the FIFO tail (loop-wrap handled). Returns
 * sectors read; 0 = end of a non-looping stream OR a failed SD read (either
 * way: play out what's buffered). */
static int cdda_read_step(int want)
{
    if (s_cdda_lba >= s_cdda_end) {
        if (s_cdda_mode == 1) s_cdda_lba = s_cdda_start;   /* LOOP: restart */
        else return 0;                                     /* NORMAL: done */
    }
    uint32_t until_end = s_cdda_end - s_cdda_lba;
    int n = (want < (int)until_end) ? want : (int)until_end;
    int got = pce_cd_read_sectors_audio(s_toc, s_cdda_lba, s_cdda_sec + s_cdda_have, n);
    if (got <= 0) {
        diag("  cdda_fill READ AUDIO FAIL lba=%lu\n", (unsigned long)s_cdda_lba);
        return 0;
    }
    s_cdda_lba  += (uint32_t)got;
    s_cdda_have += got * PCE_CD_SECTOR_RAW;
    return got;
}

static void cdda_topup(int want)
{
    while (want > 0) {
        int got = cdda_read_step(want);
        if (got <= 0)
            return;
        want -= got;
    }
}

/* Compact consumed bytes to the front so the free tail is contiguous
 * (passthrough playback needs no lookback). */
static void cdda_compact(void)
{
    if (s_cdda_pos > 0) {
        memmove(s_cdda_sec, s_cdda_sec + s_cdda_pos, (size_t)(s_cdda_have - s_cdda_pos));
        s_cdda_have -= s_cdda_pos;
        s_cdda_pos   = 0;
    }
}

/* Opportunistic CD-DA prefetch for the sound-sync WAIT loop: the pacer wait
 * is dead CPU time, so use it to pull the next sectors from SD — slow frames
 * then find the FIFO already full and their fill call skips the SD read.
 * Bounded to ONE small batched read (<=2 sectors, ~0.3-0.6 ms) per call so
 * the caller re-checks the DMA counter between reads. Returns true if a read
 * was performed (false = FIFO full / not playing / stream ended). Main-loop
 * context only — the same context as pce_scsi_cdda_fill, so no races. */
bool pce_scsi_cdda_prefetch(void)
{
    if (!s_cdda_play || s_cdda_paused || !s_present)
        return false;
    cdda_compact();
    int room = (PCE_CD_SECTOR_RAW * PCE_CDDA_RING - s_cdda_have) / PCE_CD_SECTOR_RAW;
    if (room < 1)
        return false;
    return cdda_read_step(room < 2 ? room : 2) > 0;
}

/* Fill `frames` stereo int16 samples at the native CD rate (44100): a straight
 * BIT-PERFECT copy from the sector FIFO — no decimation, no filter, no
 * resampling (the mixer runs at PCE_SAMPLE_RATE = 44100 too). Replaces the old
 * 44.1k->22.05k 4-tap decimator, which halved the bandwidth and colored the
 * top end. Returns frames produced (0 = not playing). The FIFO is topped up a
 * little each call (small, even SD reads) rather than in one burst; source
 * consumption is unchanged (44100 samples/s from disc either way). */
int pce_scsi_cdda_fill(int16_t *out, int frames)
{
    if (!s_cdda_play || !s_present) return 0;
    /* One-shot: prove the audio callback actually reaches a playing CD-DA stream on
     * device (vs the command never arriving). If this never appears but SAPSP/SAPEP
     * did, the fill path isn't wired to the mixer; if it appears but you hear nothing,
     * the loss is downstream (volume/mix). */
    { static bool logged; if (!logged) { logged = true;
        diag("  cdda_fill START lba=%lu end=%lu frames=%d\n",
             (unsigned long)s_cdda_lba, (unsigned long)s_cdda_end, frames); } }

    /* Compact, then top up a bounded few sectors — small, EVEN reads, not a
     * drain-then-burst. If the sync-wait prefetch already filled the FIFO,
     * `room` is 0 and this frame pays no SD read at all. */
    cdda_compact();
    {
        int room = (PCE_CD_SECTOR_RAW * PCE_CDDA_RING - s_cdda_have) / PCE_CD_SECTOR_RAW;
        cdda_topup((room < PCE_CDDA_TOPUP) ? room : PCE_CDDA_TOPUP);
    }

    for (int i = 0; i < frames; i++) {
        if (s_cdda_pos + 4 > s_cdda_have) {
            /* FIFO dry: the stream ended, or a read could not keep up. Pad the
             * remainder with silence; stop only if the stream is genuinely over. */
            if (s_cdda_lba >= s_cdda_end && s_cdda_mode != 1) s_cdda_play = false;
            for (; i < frames; i++) { out[i * 2] = 0; out[i * 2 + 1] = 0; }
            return frames;
        }
        out[i * 2]     = (int16_t)(s_cdda_sec[s_cdda_pos]     | (s_cdda_sec[s_cdda_pos + 1] << 8));
        out[i * 2 + 1] = (int16_t)(s_cdda_sec[s_cdda_pos + 2] | (s_cdda_sec[s_cdda_pos + 3] << 8));
        s_cdda_pos += 4;                            /* one CD frame in, one sample pair out */
    }
    return frames;
}

/* Full SCSI-engine snapshot (see pce_scsi.h). */
void pce_scsi_state_get(pce_scsi_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->phase = (uint8_t)s_phase; st->db = s_db; st->message = s_message;
    st->cmd_idx = (uint8_t)s_cmd_idx;
    st->flags = (s_bsy?1:0) | (s_req?2:0) | (s_msg?4:0) | (s_cd?8:0) | (s_io?16:0) | (s_ack?32:0);
    st->reading = s_reading; st->bulk = s_bulk;
    st->adpcm_dma_active = s_adpcm_dma_active;
    memcpy(st->cmd, s_cmd, sizeof(s_cmd));
    st->din_pos = s_din_pos; st->din_len = s_din_len;
    st->read_lba = s_read_lba; st->read_remain = s_read_remain;
    st->port2 = s_port2; st->port3 = s_port3; st->adpcm_ctrl = s_adpcm_ctrl;
    st->adpcm_dma_total = s_adpcm_dma_total;
    memcpy(st->din, s_din, sizeof(s_din));
}

void pce_scsi_state_set(const pce_scsi_state_t *st)
{
    s_phase = st->phase; s_db = st->db; s_message = st->message;
    s_cmd_idx = st->cmd_idx;
    s_bsy = st->flags & 1;  s_req = (st->flags >> 1) & 1; s_msg = (st->flags >> 2) & 1;
    s_cd  = (st->flags >> 3) & 1; s_io = (st->flags >> 4) & 1; s_ack = (st->flags >> 5) & 1;
    s_reading = st->reading; s_bulk = st->bulk;
    s_adpcm_dma_active = st->adpcm_dma_active;
    memcpy(s_cmd, st->cmd, sizeof(s_cmd));
    s_din_pos = st->din_pos; s_din_len = st->din_len;
    if (s_din_pos < 0 || s_din_pos > (int)sizeof(s_din)) s_din_pos = 0;
    if (s_din_len < 0 || s_din_len > (int)sizeof(s_din)) s_din_len = 0;
    s_read_lba = st->read_lba; s_read_remain = st->read_remain;
    s_port2 = st->port2; s_port3 = st->port3; s_adpcm_ctrl = st->adpcm_ctrl;
    s_adpcm_dma_total = st->adpcm_dma_total;
    memcpy(s_din, st->din, sizeof(s_din));
    update_irq();
    diag("  SCSI restore phase=%d reading=%d remain=%lu p2=%02x p3=%02x\n",
         s_phase, (int)s_reading, (unsigned long)s_read_remain, s_port2, s_port3);
}

/* Savestate snapshot of the CD-DA stream (see pce_scsi.h). */
void pce_scsi_cdda_get(uint32_t out[PCE_SCSI_CDDA_STATE_WORDS])
{
    out[0] = (s_cdda_play ? 1u : 0u) | (s_cdda_paused ? 2u : 0u);
    out[1] = s_cdda_lba;
    out[2] = s_cdda_start;
    out[3] = s_cdda_end;
    out[4] = (uint32_t)s_cdda_mode;
}

void pce_scsi_cdda_set(const uint32_t in[PCE_SCSI_CDDA_STATE_WORDS])
{
    s_cdda_lba   = in[1];
    s_cdda_start = in[2];
    s_cdda_end   = in[3];
    s_cdda_mode  = (int)in[4];
    s_cdda_pos = 0; s_cdda_have = 0;           /* force a fresh batch load */
    s_cdda_play  = (in[0] & 1u) && s_present;
    s_cdda_paused = (in[0] & 2u) != 0;
    diag("  CDDA restore play=%d lba=%lu end=%lu mode=%d\n",
         (int)s_cdda_play, (unsigned long)s_cdda_lba, (unsigned long)s_cdda_end, s_cdda_mode);
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
        s_read_served = 0;
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
        s_cdda_pos = 0; s_cdda_have = 0;           /* force a fresh batch load */
        s_cdda_mode  = 3;
        s_cdda_play  = (s_cmd[1] != 0);
        s_cdda_paused = !s_cdda_play;    /* NEC: SAPSP w/o play = seek then PAUSE */
        diag("  CDDA SAPSP lba=%lu play=%d\n", (unsigned long)s_cdda_lba, s_cdda_play);
        send_status(STATUS_GOOD, 0);
        break;
    case 0xD9: /* SAPEP — set end position + play mode (1=loop 2=int 3=normal 0=stop) */
        s_cdda_end  = cdda_decode_pos(s_cmd);
        s_cdda_mode = s_cmd[1];
        s_cdda_play = (s_cmd[1] != 0);
        s_cdda_paused = !s_cdda_play;    /* mode 0 = stop-at-position -> PAUSED */
        s_cdda_pos = 0; s_cdda_have = 0;
        diag("  CDDA SAPEP end=%lu mode=%d\n", (unsigned long)s_cdda_end, s_cmd[1]);
        send_status(STATUS_GOOD, 0);
        break;
    case 0xDA: /* PAUSE — hold position; SUBQ must report PAUSED(2), not stopped */
        if (s_cdda_play) s_cdda_paused = true;
        s_cdda_play = false;
        send_status(STATUS_GOOD, 0);
        break;
    case 0xDD: { /* READ SUBCHANNEL Q — audio status + position (Mednafen layout).
        * The 10-byte payload matters: after a state load, games (Cotton) issue
        * SUBQ to resync their music engine to the CD position; the old
        * status-only ack handed back nothing and the music driver never
        * restarted. Layout: [0]=audio status 0/2/3, [1]=ctl/adr, [2]=track BCD,
        * [3]=index, [4-6]=MSF relative BCD, [7-9]=MSF absolute BCD. */
        uint8_t q[10] = {0};
        uint32_t alloc = s_cmd[1] ? s_cmd[1] : 10; if (alloc > 10) alloc = 10;
        /* Position: the CD-DA stream while playing/paused, else the laser head
         * at the last data sector read — a real drive's SUBQ never says lba 0
         * after it just streamed the IPL (Dynastic checks this). */
        uint32_t lba = (s_cdda_play || s_cdda_paused) ? s_cdda_lba : s_head_lba;
        q[0] = s_cdda_play ? 0x00 : (s_cdda_paused ? 0x02 : 0x03);
        int ti = (s_present && s_toc) ? pce_cd_track_at_lba(s_toc, lba) : -1;
        if (ti >= 0) {
            const pce_cd_track_t *t = &s_toc->tracks[ti];
            uint32_t rel = lba - t->start_lba;
            uint32_t abs = lba + 150;
            #define TO_BCD(v) ((uint8_t)((((v) / 10) << 4) | ((v) % 10)))
            q[1] = t->type ? 0x41 : 0x01;                 /* ctl/adr: data vs audio */
            q[2] = TO_BCD(t->number);
            q[3] = 0x01;
            q[4] = TO_BCD(rel / 4500); q[5] = TO_BCD((rel % 4500) / 75); q[6] = TO_BCD(rel % 75);
            q[7] = TO_BCD(abs / 4500); q[8] = TO_BCD((abs % 4500) / 75); q[9] = TO_BCD(abs % 75);
            #undef TO_BCD
        }
        diag("  SUBQ st=%02x trk=%02x lba=%lu\n", q[0], q[2], (unsigned long)lba);
        do_data_in(q, alloc);
        break;
    }
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
        /* pc = the System Card ROM address doing the poll — disassemble syscard3.pce there
         * to see exactly which $1803/$1802 bit its post-read wait loop is stuck on. */
        diag("Ir%x db=%02x p3=%02x p2=%02x pc=%04x\n", reg & 0xf, s_db, s_port3, s_port2, CPU_PCE.PC);
        s_atrace++;
    }
    switch (reg & 0x0F) {
    case 0x00:
        return (uint8_t)((s_bsy ? 0x80 : 0) | (s_req ? 0x40 : 0) | (s_msg ? 0x20 : 0)
                       | (s_cd ? 0x10 : 0) | (s_io ? 0x08 : 0));
    case 0x01: return s_db;   /* command/TOC/status byte; advances on ACK */
    case 0x02: return s_port2;
    case 0x03: {
        /* $1803 IRQ status. DATA_DONE (0x20) is READ-TO-CLEAR once the bus is idle: the
         * System Card's read-completion routine polls $1803 for DATA_DONE to SET *and then
         * CLEAR* before issuing the next command. We assert it at BUSFREE but were holding
         * it high until the next SEL, so the "wait for clear" spun forever right after the
         * IPL read — the device "PUSH RUN BUTTON -> read 3596/3598 -> back to PUSH RUN"
         * loop. Return it once (satisfies wait-for-set), then clear (satisfies wait-for-
         * clear). Only in BUSFREE, so an in-flight transfer's DATA_READY is untouched. */
        uint8_t v = effective_port3();   /* incl. live ADPCM-end bit 0x08 */
        if (s_phase == PH_BUSFREE && (s_port3 & IRQ_DATA_DONE)) {
            s_port3 &= ~IRQ_DATA_DONE;
            update_irq();
        }
        return v;
    }
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
        { bool was_playing = pce_adpcm_playing();
          pce_adpcm_write(reg, val);   /* ADPCM addr/data/control/rate */
          /* Confirm the just-enabled ADPCM engine actually STARTS playing on device
           * (the DMA "ADPCM drain" only proves data landed in RAM, not that $180D
           * triggered decode). No line here = voice/SFX silent because playback never
           * started, not because of the mixer. */
          if (!was_playing && pce_adpcm_playing())
              diag("  ADPCM PLAY start reg=%02x val=%02x freq=n/a\n", reg, val); }
        break;
    case 0x0B: /* ADPCM DMA control: bit1 = enable SCSI->ADPCM auto-transfer */
        s_adpcm_ctrl = val;
        if ((val & 0x02) && s_reading && !s_adpcm_dma_active) {
            s_adpcm_dma_active = true;      /* pumped per frame by pce_scsi_run */
            s_adpcm_dma_total = 0;
            /* the bus already PRESENTS the sector's first byte (feed_din ran at
             * READ execute); rewind so the pump starts from it. Skipping it
             * shifted the whole ADPCM image one byte vs disc — fatal for games
             * that stream structured data through ADPCM RAM (Dynastic Hero),
             * merely noisy for voice samples. */
            if (s_req && s_din_pos > 0) s_din_pos--;
        }
        break;
    case 0x04: /* reset */
        if (val & 0x02) {
            diag("  SCSI RST pulse ($1804=%02x) pc=%04x cdda_play=%d lba=%lu\n",
                 val, CPU_PCE.PC, (int)s_cdda_play, (unsigned long)s_cdda_lba);
            pce_scsi_reset();
        }
        break;
    default:
        break;
    }
}

/* Per-frame hook from the main loop: pump any active SCSI->ADPCM DMA, and
 * refresh the IRQ2 level (the ADPCM-end bit can rise between register writes). */
void pce_scsi_run(void) { adpcm_dma_pump(); update_irq(); }
