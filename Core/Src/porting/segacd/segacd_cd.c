/* Sega/Mega CD — disc drive (CDD) + data controller (CDC) + BIOS, phase 3.
 *
 * Behavioral reference: PicoDrive pd_cd/cdd.c, cdc.c. SD I/O reuses the PCE-CD
 * pattern (Core/Src/porting/pce/pce_cd.c): a PERSISTENT FILE* per .bin and a
 * cached read offset, because fopen/lseek per sector is a FatFs directory walk
 * and CD-DA streams ~75 sectors/s. Only raw bin/cue/iso — no CHD (research:
 * CHD wants a 256 KB hunk buffer we cannot spare).
 *
 * Data path: CDD seeks/reads a 2048/2352-byte sector from the image into the
 * CDC's 16 KB ring; the CDC then DMAs it to PRG-RAM / Word-RAM / PCM-RAM under
 * gate-array control. CD-DA tracks stream separately to the audio mixer.
 */
#include <stdio.h>
#include <string.h>
#include "segacd.h"

#define CD_SECTOR_DATA   2048
#define CD_SECTOR_RAW    2352
#define CDC_RING_SIZE    0x4000        /* 16 KB, matches PicoDrive cdc.ram */
#define CD_MAX_TRACKS    100
/* A CUE's TRACK entries share whichever FILE line precedes them — a
 * single-BIN cue (the common case) has ONE distinct path repeated across all
 * of its (up to 100) TRACK lines. Storing a full 256-byte path per track
 * (25.6 KB) duplicates that path up to 100x; a small shared table + a 1-byte
 * index per track holds the same information for a fraction of the RAM_EMU
 * budget. 16 distinct FILE lines is generous headroom for even a multi-BIN
 * cue. */
#define CD_MAX_FILES     16

typedef struct {
    uint32_t start_lba;
    uint32_t length_lba;
    uint32_t file_offset;
    uint16_t sector_size;    /* 2048 or 2352 */
    uint8_t  is_audio;
    uint8_t  file_idx;       /* index into segacd_cd_t.file_paths */
} cd_track_t;

typedef struct {
    cd_track_t tracks[CD_MAX_TRACKS];
    int        num_tracks;
    uint32_t   total_lba;

    char       file_paths[CD_MAX_FILES][256]; /* shared table, see CD_MAX_FILES */
    int        num_files;

    FILE      *fh;                 /* persistent handle to the currently-open bin */
    char       fh_path[256];       /* which bin fh points at (avoid reopen) */
    uint32_t   fh_pos;             /* cached byte offset (skip lseek if sequential) */

    int32_t    cur_lba;            /* CDD head position — SIGNED: playback starts
                                    * 3 sectors BEFORE the target, so it walks
                                    * through pre-target (negative) LBAs. The
                                    * sub-BIOS CDC position gate only enables
                                    * buffer-write (WRRQ) while the head is 1-4
                                    * sectors ahead of the wanted sector, i.e.
                                    * it must reach the target from behind
                                    * (pd_cd/cdd.c uses a signed lba too). */
    int        status;            /* CDD status (STOP/PLAY/SEEK/READY...) */
    int        index;             /* current track index (CDD "RS2-RS3" reports) */
    int        latency;           /* CDD ticks left before a SEEK/PLAY settles */
    int        pending_play;      /* latency elapses into PLAY (1) or READY (0) */

    uint8_t    cdc_ram[CDC_RING_SIZE];
    int        cdc_head;

    /* CD-DA (Red Book audio) streaming — a raw audio sector is 2352 B = 588
     * stereo 16-bit frames. Mirrors PCE pce_scsi_cdda. */
    int        cdda_playing;
    uint32_t   cdda_lba;
    uint8_t    cdda_sec[CD_SECTOR_RAW];
    int        cdda_sec_pos;   /* byte offset consumed within cdda_sec */

    int        opened;
} segacd_cd_t;

static segacd_cd_t CD;

/* ---- HLE fast-boot gate injection (SEGACD_BOOT_CROSSING_RE.md §7) ----
 * Gate 3 ($FF8020 = s68k_regs[0x20]): disc-present flag byte. BIOS checks
 * $FE3A == 0x40 (bit6 = disc-present). Sub-BIOS overwrites $FF8020 every
 * CDD response, so we must re-inject continuously until boot mode 0x10. */
int scd_fast_boot = 0;          /* set by SCD_FAST_BOOT env var */
unsigned int scd_boot_mode = 0; /* updated per-frame by harness (M68K_RAM[$FFFDDA]) */
#define SCD_BOOT_MODE_LOGO 0x10 /* target: LOGO screen = crossing success */

/* Diagnostic: track how often segacd_cd_update() passes the gate */
uint32_t scd_cdupd_pass = 0;
uint32_t scd_cdupd_feed = 0;

/* CDD status codes — the exact values real hardware/firmware expect in RS0,
 * not arbitrary: mirrors pd_cd/cdd.h NO_DISC/CD_PLAY/CD_SEEK/CD_SCAN/
 * CD_READY/CD_OPEN/CD_STOP/CD_END. (An earlier version of this enum used
 * invented values — CDD_STOP=0, CDD_READY=9 — inverted from real hardware;
 * a sub-BIOS that branches on the actual status byte would misbehave.) */
enum { CDD_NODISC = 0x00, CDD_PLAY = 0x01, CDD_SEEK = 0x02, CDD_SCAN = 0x03,
       CDD_READY = 0x04, CDD_OPEN = 0x05, CDD_STOP = 0x09, CDD_END = 0x0C };

/* ---- cue/TOC parse (faithfully mirrors pce_cd.c:pce_cd_parse_cue) ---- */

/* Resolve a .cue FILE reference (relative) against the cue's own directory. */
static void resolve_bin_path(const char *cue_path, const char *name, char *out, size_t out_size)
{
    const char *slash = strrchr(cue_path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - cue_path) + 1;   /* keep trailing '/' */
        if (dir_len >= out_size) dir_len = out_size - 1;
        memcpy(out, cue_path, dir_len);
        out[dir_len] = '\0';
        strncat(out, name, out_size - strlen(out) - 1);
    } else {
        snprintf(out, out_size, "%s", name);
    }
}

static long file_size_bytes(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

static char *cue_trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int parse_cue(const char *cue_path)
{
    FILE *cue = fopen(cue_path, "rb");
    if (!cue) return -1;

    char     cur_bin[256] = {0};
    int      cur_file_idx = -1;
    uint32_t cur_file_base_lba = 0;   /* absolute LBA at offset 0 of cur_bin */
    uint32_t running_lba = 0;         /* total sectors across files seen so far */
    int      ti = -1;
    char     line[512];
    CD.num_tracks = 0;
    CD.num_files  = 0;

    while (fgets(line, sizeof(line), cue)) {
        char *p = cue_trim(line);

        if (!strncmp(p, "FILE", 4)) {
            const char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) continue;
            char name[256];
            size_t n = (size_t)(q2 - q1 - 1);
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, q1 + 1, n); name[n] = '\0';

            resolve_bin_path(cue_path, name, cur_bin, sizeof(cur_bin));
            cur_file_base_lba = running_lba;               /* this FILE starts here */
            long sz = file_size_bytes(cur_bin);
            if (sz > 0) running_lba += (uint32_t)(sz / CD_SECTOR_RAW);
            if (CD.num_files < CD_MAX_FILES) {
                cur_file_idx = CD.num_files++;
                snprintf(CD.file_paths[cur_file_idx], sizeof(CD.file_paths[cur_file_idx]), "%s", cur_bin);
            }
        } else if (!strncmp(p, "TRACK", 5) && CD.num_tracks < CD_MAX_TRACKS) {
            int num = 0; char mode[32] = {0};
            if (sscanf(p, "TRACK %d %31s", &num, mode) != 2) continue;
            ti = CD.num_tracks++;
            cd_track_t *t = &CD.tracks[ti];
            memset(t, 0, sizeof(*t));
            t->file_idx    = (uint8_t)(cur_file_idx >= 0 ? cur_file_idx : 0);
            t->is_audio    = (strncmp(mode, "AUDIO", 5) == 0);
            t->sector_size = strstr(mode, "/2048") ? CD_SECTOR_DATA : CD_SECTOR_RAW;
        } else if (!strncmp(p, "INDEX", 5) && ti >= 0) {
            int idx = 0, mm = 0, ss = 0, ff = 0;
            if (sscanf(p, "INDEX %d %d:%d:%d", &idx, &mm, &ss, &ff) != 4) continue;
            if (idx != 1) continue;                        /* INDEX 01 = track start */
            uint32_t frames = (uint32_t)((mm * 60 + ss) * 75 + ff);
            cd_track_t *t = &CD.tracks[ti];
            t->start_lba   = cur_file_base_lba + frames;
            t->file_offset = frames * t->sector_size;
        }
    }
    fclose(cue);

    if (CD.num_tracks == 0) return -1;

    CD.total_lba = running_lba;
    for (int i = 0; i < CD.num_tracks; i++) {
        uint32_t end = (i + 1 < CD.num_tracks) ? CD.tracks[i + 1].start_lba : CD.total_lba;
        CD.tracks[i].length_lba = end > CD.tracks[i].start_lba ? end - CD.tracks[i].start_lba : 0;
    }
    return 0;
}

static cd_track_t *track_at_lba(uint32_t lba)
{
    for (int i = 0; i < CD.num_tracks; i++) {
        cd_track_t *t = &CD.tracks[i];
        if (lba >= t->start_lba && lba < t->start_lba + t->length_lba) return t;
    }
    return CD.num_tracks ? &CD.tracks[0] : NULL;
}

static int track_index_at_lba(uint32_t lba)
{
    for (int i = 0; i < CD.num_tracks; i++) {
        cd_track_t *t = &CD.tracks[i];
        if (lba >= t->start_lba && lba < t->start_lba + t->length_lba) return i;
    }
    return CD.num_tracks > 0 ? CD.num_tracks - 1 : 0;
}

/* ---- persistent-handle sector read (the reusable PCE-CD trick) ---- */

static int read_sector(uint32_t lba, uint8_t *dst, int want)
{
    cd_track_t *t = track_at_lba(lba);
    if (!t) return -1;

    const char *bin_path = CD.file_paths[t->file_idx];
    if (CD.fh == NULL || strcmp(CD.fh_path, bin_path) != 0) {
        if (CD.fh) fclose(CD.fh);
        CD.fh = fopen(bin_path, "rb");
        if (!CD.fh) return -1;
        snprintf(CD.fh_path, sizeof(CD.fh_path), "%s", bin_path);
        CD.fh_pos = 0xFFFFFFFFu;
    }

    uint32_t off = t->file_offset + (lba - t->start_lba) * t->sector_size;
    if (t->sector_size == CD_SECTOR_RAW && want == CD_SECTOR_DATA)
        off += 16;  /* skip sync+header to reach 2048 user bytes */

    if (off != CD.fh_pos) { fseek(CD.fh, (long)off, SEEK_SET); }
    size_t got = fread(dst, 1, (size_t)want, CD.fh);
    CD.fh_pos = off + (uint32_t)got;
    return got == (size_t)want ? 0 : -1;
}

/* ---- public API ---- */

int segacd_cd_open(const char *cue_path)
{
    memset(&CD, 0, sizeof(CD));
    if (parse_cue(cue_path) != 0) return -1;
    /* NO_DISC until the sub issues its first CDD command (Stop/Read TOC),
     * exactly like real hardware — pd_cd/cdd.c:461 `cdd.status = NO_DISC;`
     * after cdd_load(). The 10-byte protocol itself drives the STOP
     * transition (segacd_cdd_command() case 0x01/0x02). */
    CD.status = CDD_NODISC;
    CD.opened = 1;
    segacd_cdc_reset();
    return 0;
}

/* Load the region BIOS (128 KB) into the main-CPU boot area. The Mega CD boots
 * the MAIN 68K from BIOS, not a cartridge. TODO(ph3b): wire main map $000000
 * to this buffer instead of the cart. */
int segacd_load_bios(const char *bios_path, uint8_t *dst, int max)
{
    FILE *f = fopen(bios_path, "rb");
    if (!f) return -1;
    size_t n = fread(dst, 1, (size_t)max, f);
    fclose(f);
    return n > 0 ? 0 : -1;
}

/* ---- backup RAM (BRAM) persistence — PCE pce_sram_load/save pattern ---- */

/* Empty formatted internal backup RAM, byte-for-byte from PicoDrive's
 * pico/cd/misc.c formatted_bram.  Mega CD software checks this 64-byte footer;
 * an all-zero battery RAM is not an empty filesystem, it is unformatted media. */
static const uint8_t formatted_bram[64] = {
    0x5f, 0x5f, 0x5f, 0x5f, 0x5f, 0x5f, 0x5f, 0x5f,
    0x5f, 0x5f, 0x5f, 0x00, 0x00, 0x00, 0x00, 0x40,
    0x00, 0x7d, 0x00, 0x7d, 0x00, 0x7d, 0x00, 0x7d,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x53, 0x45, 0x47, 0x41, 0x5f, 0x43, 0x44, 0x5f,
    0x52, 0x4f, 0x4d, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x52, 0x41, 0x4d, 0x5f, 0x43, 0x41, 0x52, 0x54,
    0x52, 0x49, 0x44, 0x47, 0x45, 0x5f, 0x5f, 0x5f,
};

static void segacd_bram_format(void)
{
    memset(SCD.bram, 0, sizeof(SCD.bram));
    memcpy(SCD.bram + sizeof(SCD.bram) - sizeof(formatted_bram),
           formatted_bram, sizeof(formatted_bram));
}

int segacd_bram_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { segacd_bram_format(); return -1; }
    size_t n = fread(SCD.bram, 1, sizeof(SCD.bram), f);
    fclose(f);
    if (n < sizeof(SCD.bram)) memset(SCD.bram + n, 0, sizeof(SCD.bram) - n);
    return 0;
}

int segacd_bram_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(SCD.bram, 1, sizeof(SCD.bram), f);
    fclose(f);
    return 0;
}

/* CDC DMA one sector from the ring into a destination (PRG/Word/PCM) selected
 * by the gate-array DMA registers. TODO(ph3c): honor DSADDR/dest-select bits. */
void segacd_cdc_dma_sector(uint8_t *dst, int len)
{
    if (!dst || len <= 0) return;
#ifdef SEGACD_GA_TRACE
    { extern uint32_t scd_dbg_dma_sector; scd_dbg_dma_sector++; }
#endif
    int n = len < CDC_RING_SIZE ? len : CDC_RING_SIZE;
    memcpy(dst, CD.cdc_ram, (size_t)n);
}

/* ---- CDD (disc drive) 10-byte command/status protocol ----
 *
 * Command:  $FF8042-$FF804B, sub-written. c[0] low nibble selects the
 *           command; writing the LAST byte ($FF804B) is the hardware trigger
 *           that fires processing (segacd_bus.c sub_ff_write8, mirrors
 *           pd_cd/memory.c:492-510 `case 0x4b: ...; cdd_process();`).
 * Status:   $FF8038-$FF8041, we fill. Byte $FF8041 is a 4-bit checksum.
 *
 * Each byte of both packets holds ONE decimal digit (0-9), not a packed BCD
 * nibble pair — mirrors PicoDrive's own convention (`c[0]*10 + c[1]` to read
 * a two-digit BCD field, `set_reg16(r, lut_BCD_16[v])` to write one; see
 * pd_cd/cdd.c:852-1229 `cdd_process()`, the behavioral reference this is
 * adapted from). Latency uses a flat tick count instead of PicoDrive's
 * distance-proportional model — adequate for a boot harness; revisit once
 * seek timing actually matters for game compatibility. */

/* Two adjacent status/command bytes hold a 0-99 value as separate decimal
 * digits (tens, ones) — arithmetic equivalent of PicoDrive's 100-entry
 * lut_BCD_16[] + set_reg16() (pd_cd/cdd.c:70-82,846-850). */
static void set_status_pair(int reg, int val)
{
    if (val < 0) val = 0;
    if (val > 99) val = 99;
    SCD.s68k_regs[reg]     = (uint8_t)(val / 10);
    SCD.s68k_regs[reg + 1] = (uint8_t)(val % 10);
}

/* Sum the 9 status digits, checksum = ~sum & 0xf into the 10th byte.
 * pd_cd/cdd.c:1222-1228. */
static void cdd_status_checksum(void)
{
    uint8_t *s = &SCD.s68k_regs[0x38];
    unsigned sum = s[0]+s[1]+s[2]+s[3]+s[4]+s[5]+s[6]+s[7]+s[8];
    s[9] = (uint8_t)(~sum & 0x0f);
}

/* Decode and respond to one 10-byte CDD command. Called when the sub writes
 * the trigger byte $FF804B (segacd_bus.c). */
#ifdef SEGACD_GA_TRACE
uint32_t scd_dbg_cdd_cmd_hist[16];
/* CDC data-path visibility (0716 boot-stall): where does disc data get stuck? */
uint32_t scd_dbg_dec_calls;    /* cdc_decoder_update entered with DECEN set */
uint32_t scd_dbg_dec_wrrq;     /* ...and WRRQ set -> sector written to ring */
uint32_t scd_dbg_cdupd_read;   /* segacd_cd_update read a data sector */
uint32_t scd_dbg_host_sub;     /* $FF8008 host-port reads by the sub */
uint32_t scd_dbg_host_sub_adv; /* ...that actually advanced (dir==3, real xfer) */
uint32_t scd_dbg_host_main;    /* $A12008 host-port reads by the main */
uint32_t scd_dbg_dma_sector;   /* segacd_cdc_dma_sector (the stubbed DMA) calls */
uint32_t scd_dbg_ctrl0_w;      /* sub writes to CTRL0 */
uint32_t scd_dbg_ctrl0_wrrq;   /* ...that set WRRQ (buffer-write request) */
#endif
/* Unconditional DMA trace (0718 PRG-RAM DMA wiring): counts DTRG arming and
 * actual firings — visible in the default build_bench.sh (no -DGA_TRACE). */
uint32_t scd_dbg_dtrg_dma;     /* DTRG writes with DDS=4/5/7 (DMA armed) */
uint32_t scd_dbg_dma_fire;     /* segacd_cdc_dma_update armed-DMA firings */

void segacd_cdd_command(void)
{
    uint8_t *c = &SCD.s68k_regs[0x42];
    uint8_t *s = &SCD.s68k_regs[0x38];
    uint8_t cmd = c[0] & 0x0f;
#ifdef SEGACD_GA_TRACE
    scd_dbg_cdd_cmd_hist[cmd]++;
#endif

    switch (cmd) {
    case 0x00: {  /* Drive Status — current status + absolute head position */
        cd_track_t *t = track_at_lba(CD.cur_lba);
        int lba = (int)CD.cur_lba + 150;
        s[0] = (uint8_t)CD.status;
        s[1] = 0x00;
        set_status_pair(0x3a, lba/75/60);
        set_status_pair(0x3c, (lba/75)%60);
        set_status_pair(0x3e, lba%75);
        s[8] = (uint8_t)((t && !t->is_audio) ? 0x04 : 0x00);
        break;
    }

    case 0x01:  /* Stop Drive — RS1-RS8 ignored, report all-zero/0xf per spec */
        CD.status = CD.opened ? CDD_STOP : CDD_NODISC;
        CD.cur_lba = 0; CD.index = 0; CD.latency = 0;
        s[0] = (uint8_t)CD.status; s[1] = 0; s[2] = 0; s[3] = 0;
        s[4] = 0; s[5] = 0; s[6] = 0; s[7] = 0; s[8] = 0x0f;
        break;

    case 0x02:  /* Read TOC — c[3] ($FF8045) selects which field (Q-channel
                 * infos); c[1] ($FF8043) is a reserved always-0 byte —
                 * pd_cd/cdd.c:913 reads s68k_regs[0x44+1] = c[3]. */
        if (CD.status == CDD_NODISC)
            CD.status = CD.opened ? CDD_STOP : CDD_NODISC;
        switch (c[3] & 0x0f) {
        case 0x00: {  /* current absolute time (MM:SS:FF) */
            cd_track_t *t = track_at_lba(CD.cur_lba);
            int lba = (int)CD.cur_lba + 150;
            s[0] = (uint8_t)CD.status; s[1] = 0x00;
            set_status_pair(0x3a, lba/75/60);
            set_status_pair(0x3c, (lba/75)%60);
            set_status_pair(0x3e, lba%75);
            s[8] = (uint8_t)((t && !t->is_audio) ? 0x04 : 0x00);
            break;
        }
        case 0x01: {  /* current track relative time */
            cd_track_t *t = track_at_lba(CD.cur_lba);
            int lba = t ? (int)(CD.cur_lba - t->start_lba) : 0;
            if (lba < 0) lba = 0;
            s[0] = (uint8_t)CD.status; s[1] = 0x01;
            set_status_pair(0x3a, lba/75/60);
            set_status_pair(0x3c, (lba/75)%60);
            set_status_pair(0x3e, lba%75);
            s[8] = (uint8_t)((t && !t->is_audio) ? 0x04 : 0x00);
            break;
        }
        case 0x02:  /* current track number */
            s[0] = (uint8_t)CD.status; s[1] = 0x02;
            set_status_pair(0x3a, track_index_at_lba(CD.cur_lba) + 1);
            s[4] = 0; s[5] = 0; s[6] = 0; s[7] = 0; s[8] = 0;
            break;
        case 0x03: {  /* total disc length */
            int lba = (int)CD.total_lba + 150;
            s[0] = (uint8_t)CD.status; s[1] = 0x03;
            set_status_pair(0x3a, lba/75/60);
            set_status_pair(0x3c, (lba/75)%60);
            set_status_pair(0x3e, lba%75);
            s[8] = 0;
            break;
        }
        case 0x04:  /* first & last track numbers */
            s[0] = (uint8_t)CD.status; s[1] = 0x04;
            set_status_pair(0x3a, 1);
            set_status_pair(0x3c, CD.num_tracks);
            s[6] = 0; s[7] = 0; s[8] = 0;
            break;
        case 0x05: {  /* track start time; track number requested in c[4..5]
                       * ($FF8046-$FF8047) — pd_cd/cdd.c:971 uses
                       * s68k_regs[0x46+0]*10 + s68k_regs[0x46+1] = c[4],c[5]. */
            int trk = c[4]*10 + c[5];
            int lba = (trk >= 1 && trk <= CD.num_tracks)
                        ? (int)CD.tracks[trk-1].start_lba + 150 : 150;
            s[0] = (uint8_t)CD.status; s[1] = 0x05;
            set_status_pair(0x3a, lba/75/60);
            set_status_pair(0x3c, (lba/75)%60);
            set_status_pair(0x3e, lba%75);
            s[8] = (uint8_t)(trk % 10);
            if (trk == 1) s[6] |= 0x08;   /* bit3: first (DATA) track */
            break;
        }
        default:  /* 0x06 latest error, and anything unhandled: zeroed */
            s[0] = (uint8_t)CD.status; s[1] = c[3] & 0x0f;
            s[2] = 0; s[3] = 0; s[4] = 0; s[5] = 0; s[6] = 0; s[7] = 0; s[8] = 0;
            break;
        }
        break;

    case 0x03:    /* Play from LBA */
    case 0x04: {  /* Seek to LBA — same addressing, only the settle state differs */
        int lba = ((c[2]*10+c[3])*60 + (c[4]*10+c[5]))*75 + (c[6]*10+c[7]) - 150;
        if (lba < 0) lba = 0;
        CD.index        = track_index_at_lba((uint32_t)lba);
        /* Start 3 sectors early WITHOUT clamping to 0 — for the boot read
         * (target LBA 0) the head must walk 147,148,149 (abs) before 150 so
         * the sub-BIOS's CDC position gate (target-current in [1,4]) fires and
         * sets WRRQ. Clamping to 0 made the head start AT the target, the gate
         * never matched, WRRQ never set, and the boot hung. pd_cd/cdd.c seeks
         * to (lba-3) with a signed lba. Verified against a GPGX boot trace:
         * WRRQ first asserts at head LBA -1. */
        CD.cur_lba      = lba - 3;
        CD.latency      = 2;      /* flat settle delay (CDD ticks); see file header note */
        CD.pending_play = (cmd == 0x03);
        CD.status       = CDD_SEEK;
        /* RS1=0x0f invalidates RS2-RS8 while seeking — pd_cd/cdd.c:1080-1085 */
        s[0] = CDD_SEEK; s[1] = 0x0f;
        s[2] = 0; s[3] = 0; s[4] = 0; s[5] = 0; s[6] = 0; s[7] = 0;
        s[8] = (uint8_t)(~(CDD_SEEK + 0xf) & 0x0f);
        break;
    }

    case 0x06:  /* Pause — RS1-RS8 left as-is */
        CD.status = CDD_READY;
        s[0] = (uint8_t)CD.status;
        break;

    case 0x07:  /* Resume — RS1-RS8 left as-is */
        CD.status = CDD_PLAY;
        s[0] = (uint8_t)CD.status;
        break;

    default:   /* Scan/track-jump/tray control — not modelled; echo status */
        s[0] = (uint8_t)CD.status;
        break;
    }

    cdd_status_checksum();
    segacd_poll_wake();   /* CDD status changed — a spin-waiting sub must re-check */

    /* PM-directed side-by-side CDD cmd=02 trace (matches PicoDrive format). */
    if (c[0] == 0x02) {
        static unsigned int h_cdd02_count = 0;
        if (h_cdd02_count < 40) {
            printf("[CDD02] #%u sub=%X st=%02X lba=%u idx=%u rs=%02X%02X %02X%02X %02X%02X %02X%02X %02X ien=%02X pc=%06X\n",
                   h_cdd02_count,
                   c[3] & 0x0f,
                   CD.status, CD.cur_lba, CD.index,
                   s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8],
                   SCD.s68k_regs[0x33],
                   m68k_get_reg(15));  /* M68K_REG_PC=15 in Musashi-like enum */
            h_cdd02_count++;
        }
    }

    /* HLE fast-boot: re-inject gate 3 after sub-BIOS command response
     * (sub-BIOS just wrote its own status to $FF8020). §7.3 caveat 2. */
    if (scd_fast_boot && scd_boot_mode < SCD_BOOT_MODE_LOGO) {
        SCD.s68k_regs[0x20] = 0x40;  /* disc-present flag */
    }
}

/* ---- Subchannel Q synthesis ----
 * Real CD hardware: the servo reads subchannel Q from each sector's
 * subchannel bytes and feeds it to the sub-68K via a buffer at $FF8100
 * with a write index at $FF8069. The level-4 ISR ($2532) reads the index,
 * validates it (bit7 clear AND index >= 0x62 after debounce subtract),
 * then copies 96 bytes of Q data from $FF8100+index into a ring buffer.
 *
 * Without this feed, the ISR debounce path always fails ($FF8069 == 0),
 * the sub-BIOS never gets subchannel Q updates, and the CDD command retry
 * loop ($7028) never validates the disc → sub never sends cmd 3 (Play)
 * → CDC DMA never fires → no game data reaches PRG-RAM.
 *
 * We synthesize Q data the same way real hardware does: compute track /
 * index / relative-MSF / absolute-MSF from the cue sheet's track table
 * and the current CDD head position. CRC-16 per ISO 10149 (CRC-CCITT
 * poly 0x1021, init 0, complemented). */

static uint16_t q_crc16(const uint8_t *d, int len)
{
    uint16_t crc = 0x0000;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
            else              crc = (uint16_t)(crc << 1);
        }
    }
    return (uint16_t)(~crc);
}

/* Subcode Q-channel packed BCD (CD Red Book): one byte holds both decimal
 * digits, nibble-packed (v=25 -> 0x25). This is the correct, real-hardware
 * encoding for Q-channel data — do NOT "fix" it to match cdd_put_bcd_digits()
 * below. The CDD *serial command interface* (RS0-RS8) uses a DIFFERENT,
 * unrelated convention: each decimal digit gets its own byte (see
 * cdd_put_bcd_digits). Two genuinely different BCD conventions coexist in
 * this file because they're two different hardware interfaces — an earlier
 * version of this file conflated them (0720, cmd=2 subcommand responses were
 * built with this function's nibble-packing instead of digit-per-byte,
 * which stalled the sub-BIOS's TOC scan forever). */
static uint8_t to_bcd(int v)
{
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* CDD serial command interface BCD: each decimal digit (0-9) gets its own
 * byte, NOT nibble-packed like to_bcd() above — real hardware and reference
 * emulators (PicoDrive's lut_BCD_16) split e.g. v=25 into rs[idx]=0x02,
 * rs[idx+1]=0x05. Writing the same nibble-packed byte into both halves (the
 * old bug here) is byte-identical to the correct encoding only for v<10,
 * which is why it went undetected through several fields/frames before the
 * first two-digit value exposed it. */
static void cdd_put_bcd_digits(uint8_t *rs, int idx, int v)
{
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    rs[idx]     = (uint8_t)(v / 10);
    rs[idx + 1] = (uint8_t)(v % 10);
}

void segacd_subcode_q_update(void)
{
    if (!CD.opened || CD.num_tracks == 0) return;

    /* CDD head position. cur_lba is signed (can be negative during seek-in). */
    int32_t lba = CD.cur_lba;
    if (lba < 0) lba = 0;

    cd_track_t *t = track_at_lba((uint32_t)lba);
    if (!t) t = &CD.tracks[0];
    int track_no = (int)(t - CD.tracks) + 1;

    /* Q-word (12 bytes): control/ADR, track BCD, index BCD, rel MSF BCD,
     * reserved, abs MSF BCD, CRC-16 big-endian. */
    uint8_t q[12];
    /* Control/ADR: bit2 of control = data track (4-bit data); ADR=1 (Q-Mode1).
     * Audio tracks: control=0x00. Data track: control=0x40. */
    q[0] = (uint8_t)(t->is_audio ? 0x01 : 0x41);
    q[1] = to_bcd(track_no);
    q[2] = 0x01;                         /* index 01 (normal playback) */

    /* Relative MSF within track (from track start). */
    int32_t rel = lba - (int32_t)t->start_lba;
    if (rel < 0) rel = 0;
    int rel_mm = (rel / 75) / 60;  if (rel_mm > 99) rel_mm = 99;
    int rel_ss = (rel / 75) % 60;
    int rel_ff =  rel % 75;
    q[3] = to_bcd(rel_mm);
    q[4] = to_bcd(rel_ss);
    q[5] = to_bcd(rel_ff);
    q[6] = 0x00;                         /* reserved (frame within pregap) */

    /* Absolute MSF from disc start (LBA + 150 frames pregap). */
    int32_t abl = lba + 150;
    int abs_mm = (abl / 75) / 60;  if (abs_mm > 99) abs_mm = 99;
    int abs_ss = (abl / 75) % 60;
    int abs_ff =  abl % 75;
    q[7]  = to_bcd(abs_mm);
    q[8]  = to_bcd(abs_ss);
    q[9]  = to_bcd(abs_ff);

    /* CRC-16 over bytes 0..9, complemented, big-endian. */
    uint16_t crc = q_crc16(q, 10);
    q[10] = (uint8_t)(crc >> 8);
    q[11] = (uint8_t)(crc & 0xFF);

    /* Replicate the Q-word across the entire $FF8100-$FF81FF buffer
     * (256 bytes = 21 Q-words + 4 pad). The ISR reads 96 bytes from
     * $FF8100+index, so any starting offset within the buffer yields
     * valid data. */
    uint8_t *buf = &SCD.s68k_regs[0x100];
    for (int i = 0; i < SEGACD_GA_REGS - 0x100; i++) {
        buf[i] = q[i % 12];
    }

    /* $FF8069: subchannel Q write index. The ISR debounce requires:
     *   bit7 clear AND (index - $5AB6) & 0x7F >= 0x62
     * Since $5AB6 = 0x80 at init, this simplifies to index >= 0x62.
     * We set 0x62 — within the 256-byte buffer, 0x62 + 96 = 0xC2 ≤ 0xFF. */
    SCD.s68k_regs[0x69] = 0x62;
}

/* Periodic (~75 Hz) CDD tick: settles pending seeks and, while the sub has
 * armed the transfer ($FF8037 bit2 — see segacd_bus.c sub_ff_write8), keeps
 * the status packet's RS0 byte current and arms the level-4 export
 * interrupt. IEN4 gating happens in segacd_run_sub (segacd_engine.c) — real
 * hardware only actually asserts the line when both the arm bit and the
 * mask are set (pd_cd/mcd.c:190-200 `pcd_cdc_event()`). */
void segacd_cdd_process(void)
{
    if (!CD.opened) return;

    /* Feed subchannel Q data every 75Hz tick. Real hardware's servo does
     * this continuously; without it the level-4 ISR can never validate
     * disc position and the sub-BIOS never advances past TOC reading. */
    segacd_subcode_q_update();

    if (CD.latency > 0) {
        CD.latency--;
        if (CD.latency == 0) {
            CD.status = CD.pending_play ? CDD_PLAY : CDD_READY;
            SCD.s68k_regs[0x38] = (uint8_t)CD.status;
            cdd_status_checksum();
            segacd_poll_wake();
        }
    }

    if (SCD.s68k_regs[0x37] & 0x04) {
        /* PicoDrive-grade CDD status response: update RS0-RS8 every 75Hz tick
         * (pd_cd/cdd.c:852). Guard on CD.status: only fill RS1-RS8 after the
         * BIOS has actually issued a CDD command (status transitions away
         * from NODISC=0). Early boot expects RS1-RS8 = 0. */
        uint8_t *rs   = &SCD.s68k_regs[0x38];          /* RS0-RS8 */
        uint8_t  cmd  = SCD.s68k_regs[0x42] & 0x0f;    /* current CDD command */
        rs[0] = (uint8_t)CD.status;                     /* RS0 = drive status */

        if (CD.status >= CDD_PLAY) {   /* only when drive has been commanded */
            int lba = (int)CD.cur_lba + 150;
            if (lba < 0) lba = 0;
            int mm = (lba / 75) / 60; if (mm > 99) mm = 99;
            int ss = (lba / 75) % 60;
            int ff =  lba % 75;

            switch (cmd) {
            case 0x00:      /* Drive Status */
                if (rs[1] == 0x0f || rs[1] == 0x00 || rs[1] == 0x01) {
                    if (rs[1] == 0x0f) rs[1] = 0x00;
                    cdd_put_bcd_digits(rs, 2, mm);
                    cdd_put_bcd_digits(rs, 4, ss);
                    cdd_put_bcd_digits(rs, 6, ff);
                    rs[8] = 0x04;
                }
                break;
            case 0x02:      /* Read TOC */
                switch (SCD.s68k_regs[0x45] & 0x0f) {
                case 0x00:  /* absolute MS */
                    rs[1]=0x00;
                    cdd_put_bcd_digits(rs, 2, mm);
                    cdd_put_bcd_digits(rs, 4, ss);
                    cdd_put_bcd_digits(rs, 6, ff);
                    rs[8]=0x04;
                    break;
                case 0x01: { /* track-relative MS — must be relative to the
                              * current track, NOT the disc start. The cmd
                              * 0x02 handler (line ~399) gets this right; this
                              * periodic update was using absolute time, which
                              * made the sub-BIOS think the head never moved
                              * within the track and the TOC scan stalled. */
                    cd_track_t *t = track_at_lba(CD.cur_lba);
                    int32_t rel = t ? (int32_t)CD.cur_lba - (int32_t)t->start_lba : 0;
                    if (rel < 0) rel = 0;
                    int rmm = (rel / 75) / 60; if (rmm > 99) rmm = 99;
                    int rss = (rel / 75) % 60;
                    int rff =  rel % 75;
                    rs[1]=0x01;
                    cdd_put_bcd_digits(rs, 2, rmm);
                    cdd_put_bcd_digits(rs, 4, rss);
                    cdd_put_bcd_digits(rs, 6, rff);
                    rs[8]=0x04;
                    break;
                }
                case 0x02: { /* current track number — must reflect actual
                              * track at the current head position, not a
                              * hardcoded 1. As the LBA advances through
                              * track boundaries, the track number changes
                              * and the sub-BIOS records each track start. */
                    cd_track_t *t2 = track_at_lba(CD.cur_lba);
                    int trk_no = t2 ? (int)(t2 - CD.tracks) + 1 : 1;
                    rs[1]=0x02;
                    cdd_put_bcd_digits(rs, 2, trk_no);
                    rs[4]=0; rs[5]=0; rs[6]=0; rs[7]=0; rs[8]=0;
                    break;
                }
                default: break;
                }
                break;
            default: break;
            }
        }

        cdd_status_checksum();
        SCD.cdd_int_pending = 1;   /* periodic CDD IRQ (level 4) drives the sub-BIOS */
        /* Also arm the level-2 interrupt. In real hardware the CDD fires
         * INT2 at 75Hz to let the sub-BIOS process status changes (the
         * level-2 ISR at $131C→$13F6 reads CDD status $586E and sets
         * $583A, which gates the level-4 Q-processor). The harness was
         * only setting ga_ifl2 from the $A12000 doorbell write, but the
         * main 68K parks in the $FE26 spin loop and never pulses the
         * doorbell again — so the level-2 ISR never re-fired, $583A was
         * cleared by the first ISR call and never re-set, and the Q
         * processor stalled after one pass. Pulse ga_ifl2 here every
         * tick to model the 75Hz CDD INT2. */
        SCD.ga_ifl2 = 1;
    }

    /* HLE fast-boot: inject gate 3 ($FF8020 = disc-present flag 0x40).
     * Sub-BIOS overwrites this byte with its own CDD status every response,
     * so we must re-inject on every 75Hz tick until boot reaches mode 0x10. */
    if (scd_fast_boot && scd_boot_mode < SCD_BOOT_MODE_LOGO) {
        SCD.s68k_regs[0x20] = 0x40;  /* disc-present flag (bit 6) */
    }
}

/* ---- CD-DA streaming (audio tracks) ---- */

void segacd_cdda_play(uint32_t lba)
{
    CD.cdda_lba     = lba;
    CD.cdda_sec_pos = CD_SECTOR_RAW;   /* force a sector read on next fill */
    CD.cdda_playing = 1;
}

void segacd_cdda_stop(void) { CD.cdda_playing = 0; }

/* Ensure the current audio sector is resident (one small SD read). */
int segacd_cdda_prefetch(void)
{
    if (!CD.cdda_playing) return 0;
    if (CD.cdda_sec_pos >= CD_SECTOR_RAW) {
        if (read_sector(CD.cdda_lba, CD.cdda_sec, CD_SECTOR_RAW) != 0) {
            CD.cdda_playing = 0;
            return 0;
        }
        CD.cdda_lba++;
        CD.cdda_sec_pos = 0;
    }
    return 1;
}

/* Fill `frames` stereo 16-bit samples of CD-DA into dst. Returns frames done. */
int segacd_cdda_fill(int16_t *dst, int frames)
{
    if (!CD.cdda_playing) return 0;
    int done = 0;
    while (done < frames) {
        if (CD.cdda_sec_pos >= CD_SECTOR_RAW) {
            if (!segacd_cdda_prefetch()) break;
        }
        int avail = (CD_SECTOR_RAW - CD.cdda_sec_pos) / 4;   /* stereo frames left */
        int n = frames - done;
        if (n > avail) n = avail;
        memcpy(dst + done * 2, CD.cdda_sec + CD.cdda_sec_pos, (size_t)n * 4);
        CD.cdda_sec_pos += n * 4;
        done += n;
    }
    return done;
}

/* ---- CDC (data controller, LC89510-compatible) ----
 *
 * Behavioral reference: PicoDrive pd_cd/cdc.c. Minimal subset implemented:
 *   - register-index protocol: sub write $FF8005 latches the register index
 *     (segacd_bus.c sub_ff_write8 reg==5); sub write/read $FF8007 access
 *     CDC register[idx] via segacd_cdc_reg_w/r below, auto-incrementing idx
 *     exactly like cdc.c's cdc_reg_w/cdc_reg_r (:474-823).
 *   - decoder: segacd_cd_update() (75 Hz) calls cdc_decoder_update() below
 *     for the current sector, mirroring cdd_update()'s call into
 *     cdc_decoder_update() (cdc.c:408-472): when CTRL0's DECEN bit is set it
 *     captures the sector header, and if WRRQ is also set, writes
 *     header+data into the CDC ring at the PT/WA cursor and arms the level-5
 *     "DECI" interrupt (gated on IEN5, $FF8033 bit5).
 *   - host data port: MAIN reads $A12008 / SUB reads $FF8008 pull one 16-bit
 *     word via segacd_cdc_host_r() (cdc.c:825-895), advancing DAC/DBC only
 *     when the destination selected by the last DTRG (regs[4] bits 0-2 ==
 *     2 MAIN / 3 SUB) matches the caller.
 *
 * NOT modeled (deferred — add only if a GA trace shows the BIOS using it):
 *   - PRG-RAM/Word-RAM/PCM-RAM DMA destinations (regs[4] bits 0-2 == 4/5/7).
 *   - cdc.c's cycle-accurate DECI timing window (check_decoder_irq_pending's
 *     67250-cycle phase) — this engine's whole interrupt model is one-shot
 *     pulses delivered at the next sub timeslice (segacd_run_sub), not held
 *     levels, so DECI is simply armed the instant the decoder produces data.
 */
#define CDC_IFSTAT_DTEI    0x40
#define CDC_IFSTAT_DECI    0x20
#define CDC_IFSTAT_DTBSY   0x08
#define CDC_IFSTAT_DTEN    0x02

#define CDC_IFCTRL_DTEIEN  0x40
#define CDC_IFCTRL_DECIEN  0x20
#define CDC_IFCTRL_DOUTEN  0x02

#define CDC_CTRL0_DECEN    0x80
#define CDC_CTRL0_AUTORQ   0x10
#define CDC_CTRL0_WRRQ     0x04

#define CDC_CTRL1_MODRQ    0x08
#define CDC_CTRL1_FORMRQ   0x04

typedef struct {
    uint8_t  ifstat;
    uint8_t  ifctrl;
    uint16_t dbc;             /* data byte counter */
    uint16_t dac;             /* data address counter (host-read pointer) */
    uint16_t pt;              /* block pointer (decoder write cursor) */
    uint16_t wa;               /* write address (mirrors pt on decode) */
    uint8_t  ctrl0, ctrl1;
    uint8_t  head[4];          /* last decoded sector header (MM SS FF mode) */
    uint8_t  stat0, stat2, stat3;
    int     dma_w;             /* armed DMA destination (0=none, 4=PCM, 5=PRG, 7=Word) */
} segacd_cdc_t;

static segacd_cdc_t CDC;

void segacd_cdc_reset(void)
{
    memset(&CDC, 0, sizeof(CDC));
    CDC.ifstat = 0xff;
    CDC.stat3  = 0x80;             /* !VALST: no valid data yet */
    SCD.s68k_regs[0x05] = 0x00;    /* register index — cdc.c cdc_reset() */
}

static uint8_t bcd8(int v)
{
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

uint8_t segacd_cdc_reg_r(void)
{
    uint8_t idx = SCD.s68k_regs[0x05] & 0x1f;
    switch (idx) {
    case 0x00:
        SCD.s68k_regs[0x05] = 0x01;
        return 0xff;
    case 0x01:  /* IFSTAT */
        SCD.s68k_regs[0x05] = 0x02;
        return CDC.ifstat;
    case 0x02:  /* DBCL */
        SCD.s68k_regs[0x05] = 0x03;
        return (uint8_t)(CDC.dbc & 0xff);
    case 0x03:  /* DBCH */
        SCD.s68k_regs[0x05] = 0x04;
        return (uint8_t)((CDC.dbc >> 8) & 0xff);
    case 0x04:  /* HEAD0 */
        SCD.s68k_regs[0x05] = 0x05;
        return CDC.head[0];
    case 0x05:  /* HEAD1 */
        SCD.s68k_regs[0x05] = 0x06;
        return CDC.head[1];
    case 0x06:  /* HEAD2 */
        SCD.s68k_regs[0x05] = 0x07;
        return CDC.head[2];
    case 0x07:  /* HEAD3 */
        SCD.s68k_regs[0x05] = 0x08;
        return CDC.head[3];
    case 0x08:  /* PTL */
        SCD.s68k_regs[0x05] = 0x09;
        return (uint8_t)(CDC.pt & 0xff);
    case 0x09:  /* PTH */
        SCD.s68k_regs[0x05] = 0x0a;
        return (uint8_t)((CDC.pt >> 8) & 0xff);
    case 0x0a:  /* WAL */
        SCD.s68k_regs[0x05] = 0x0b;
        return (uint8_t)(CDC.wa & 0xff);
    case 0x0b:  /* WAH */
        SCD.s68k_regs[0x05] = 0x0c;
        return (uint8_t)((CDC.wa >> 8) & 0xff);
    case 0x0c:  /* STAT0 */
        SCD.s68k_regs[0x05] = 0x0d;
        return CDC.stat0;
    case 0x0d:  /* STAT1 — always 0 */
        SCD.s68k_regs[0x05] = 0x0e;
        return 0x00;
    case 0x0e:  /* STAT2 */
        SCD.s68k_regs[0x05] = 0x0f;
        return CDC.stat2;
    case 0x0f: {  /* STAT3 */
        uint8_t data = CDC.stat3;
        CDC.stat3 = 0x80;                    /* !VALST set back (cdc.c note:
                                               * "not 100% correct but BIOS
                                               * do not seem to care") */
        CDC.ifstat |= CDC_IFSTAT_DECI;       /* clear pending decoder IRQ condition */
        SCD.s68k_regs[0x05] = 0x10;
        return data;
    }
    default:  /* COMIN — always empty */
        SCD.s68k_regs[0x05] = (uint8_t)((idx + 1) & 0x1f);
        return 0xff;
    }
}

void segacd_cdc_reg_w(uint8_t data)
{
    uint8_t idx = SCD.s68k_regs[0x05] & 0x1f;
    switch (idx) {
    case 0x00:
        break;
    case 0x01:  /* IFCTRL */
        CDC.ifctrl = data;
        if (!(data & CDC_IFCTRL_DOUTEN))
            CDC.ifstat |= (uint8_t)(CDC_IFSTAT_DTBSY | CDC_IFSTAT_DTEN);
        SCD.s68k_regs[0x05] = 0x02;
        break;
    case 0x02:  /* DBCL */
        CDC.dbc = (uint16_t)((CDC.dbc & 0xff00) | data);
        SCD.s68k_regs[0x05] = 0x03;
        break;
    case 0x03:  /* DBCH */
        CDC.dbc = (uint16_t)((CDC.dbc & 0x00ff) | ((data & 0x0f) << 8));
        SCD.s68k_regs[0x05] = 0x04;
        break;
    case 0x04:  /* DACL */
        CDC.dac = (uint16_t)((CDC.dac & 0xff00) | data);
        SCD.s68k_regs[0x05] = 0x05;
        break;
    case 0x05:  /* DACH */
        CDC.dac = (uint16_t)((CDC.dac & 0x00ff) | (data << 8));
        SCD.s68k_regs[0x05] = 0x06;
        break;
    case 0x06:  /* DTRG: start data transfer if output enabled */
        if (CDC.ifctrl & CDC_IFCTRL_DOUTEN) {
            CDC.ifstat &= (uint8_t)~CDC_IFSTAT_DTBSY;
            CDC.dbc &= 0x0fff;
            SCD.s68k_regs[0x04] &= 0x07;
            switch (SCD.s68k_regs[0x04] & 0x07) {
            case 0x02: case 0x03:  /* MAIN/SUB host read */
                CDC.ifstat &= (uint8_t)~CDC_IFSTAT_DTEN;
                SCD.s68k_regs[0x04] |= 0x40;   /* set DSR */
                break;
            case 0x04:  /* PCM-RAM DMA */
            case 0x05:  /* PRG-RAM DMA */
            case 0x07:  /* Word-RAM DMA */
                /* Arm the transfer; segacd_cdc_dma_update() fires it on the
                 * next CDD tick. Mirrors pd_cd/cdc.c do_dma() — the BIOS
                 * relies on this for game-program loads ($15800/$13400/$19800
                 * before the $0656 decompressor). PRG-RAM (5) is wired
                 * through; PCM (4) / Word-RAM (7) are armed but the actual
                 * transfer is a no-op until a trace needs them. */
                CDC.dma_w = (int)(SCD.s68k_regs[0x04] & 0x07);
                CDC.ifstat &= (uint8_t)~CDC_IFSTAT_DTEN;
                SCD.s68k_regs[0x04] |= 0x40;   /* DSR */
                scd_dbg_dtrg_dma++;
                break;
            default:
                break;
            }
        }
        SCD.s68k_regs[0x05] = 0x07;
        break;
    case 0x07:  /* DTACK */
        CDC.ifstat |= CDC_IFSTAT_DTEI;
        CDC.dbc &= 0x0fff;
        SCD.s68k_regs[0x05] = 0x08;
        break;
    case 0x08:  /* WAL */
        CDC.wa = (uint16_t)((CDC.wa & 0xff00) | data);
        SCD.s68k_regs[0x05] = 0x09;
        break;
    case 0x09:  /* WAH */
        CDC.wa = (uint16_t)((CDC.wa & 0x00ff) | (data << 8));
        SCD.s68k_regs[0x05] = 0x0a;
        break;
    case 0x0a:  /* CTRL0 */
#ifdef SEGACD_GA_TRACE
        { extern uint32_t scd_dbg_ctrl0_w, scd_dbg_ctrl0_wrrq;
          scd_dbg_ctrl0_w++; if (data & CDC_CTRL0_WRRQ) scd_dbg_ctrl0_wrrq++; }
#endif
        if (!(data & CDC_CTRL0_DECEN))
            CDC.ifstat |= CDC_IFSTAT_DECI;
        CDC.stat2 = (uint8_t)((data & CDC_CTRL0_AUTORQ)
                                ? (CDC.ctrl1 & CDC_CTRL1_MODRQ)
                                : (CDC.ctrl1 & (CDC_CTRL1_MODRQ | CDC_CTRL1_FORMRQ)));
        CDC.ctrl0 = data;
        SCD.s68k_regs[0x05] = 0x0b;
        break;
    case 0x0b:  /* CTRL1 */
        CDC.stat2 = (uint8_t)((CDC.ctrl0 & CDC_CTRL0_AUTORQ)
                                ? (data & CDC_CTRL1_MODRQ)
                                : (data & (CDC_CTRL1_MODRQ | CDC_CTRL1_FORMRQ)));
        CDC.ctrl1 = data;
        SCD.s68k_regs[0x05] = 0x0c;
        break;
    case 0x0c:  /* PTL */
        CDC.pt = (uint16_t)((CDC.pt & 0xff00) | data);
        SCD.s68k_regs[0x05] = 0x0d;
        break;
    case 0x0d:  /* PTH */
        CDC.pt = (uint16_t)((CDC.pt & 0x00ff) | (data << 8));
        SCD.s68k_regs[0x05] = 0x0e;
        break;
    case 0x0e:  /* CTRL2 — unused */
        SCD.s68k_regs[0x05] = 0x0f;
        break;
    case 0x0f:  /* RESET */
        segacd_cdc_reset();
        break;
    default:
        SCD.s68k_regs[0x05] = (uint8_t)((idx + 1) & 0x1f);
        break;
    }
}

/* Host data port ($A12008 main / $FF8008 sub) — pulls one 16-bit word from
 * the CDC ring at the current DAC pointer. Only the destination selected by
 * the last DTRG (regs[4] bits 0-2 == 2 MAIN / 3 SUB) actually advances the
 * transfer; a read from the "wrong" side just peeks the same word. Mirrors
 * cdc_host_r() (cdc.c:825-895), minus the mcd-verificator DSR-sync hack
 * (that workaround exists because PicoDrive's two 68Ks run on independently
 * scheduled event loops; our sub is a full, uninterrupted timeslice inside
 * the main's frame, so there's no cross-CPU race to paper over here). */
uint16_t segacd_cdc_host_r(int sub)
{
    int dir = SCD.s68k_regs[0x04] & 0x07;
#ifdef SEGACD_GA_TRACE
    { extern uint32_t scd_dbg_host_sub, scd_dbg_host_main;
      if (sub) scd_dbg_host_sub++; else scd_dbg_host_main++; }
#endif

    if (CDC.ifstat & CDC_IFSTAT_DTEN)
        return 0xffff;   /* no data available */

    unsigned int off = CDC.dac & (CDC_RING_SIZE - 2);
    uint16_t data = (uint16_t)((CD.cdc_ram[off] << 8) | CD.cdc_ram[off + 1]);

    if ((sub && dir != 3) || (!sub && dir != 2))
        return data;     /* not the configured destination: peek only */

#ifdef SEGACD_GA_TRACE
    { extern uint32_t scd_dbg_host_sub_adv; scd_dbg_host_sub_adv++; }
#endif
    CDC.dac = (uint16_t)(CDC.dac + 2);
    CDC.dbc = (uint16_t)(CDC.dbc - 2);

    if ((int16_t)CDC.dbc <= 0) {
        CDC.dbc = 0xffff;
        CDC.ifstat |= (uint8_t)(CDC_IFSTAT_DTBSY | CDC_IFSTAT_DTEN);
        SCD.s68k_regs[0x04] = (uint8_t)((SCD.s68k_regs[0x04] & 0x07) | 0x80);   /* EDT, DSR cleared */
    } else if ((int16_t)CDC.dbc <= 2) {
        if (CDC.ifstat & CDC_IFSTAT_DTEI) {
            CDC.ifstat &= (uint8_t)~CDC_IFSTAT_DTEI;
            if (CDC.ifctrl & CDC_IFCTRL_DTEIEN)
                SCD.cdc_int_pending = 1;
        }
        SCD.s68k_regs[0x04] = (uint8_t)((SCD.s68k_regs[0x04] & 0x07) | 0xc0);   /* DSR+EDT */
    }

    return data;
}

/* One-shot CDC DMA to PRG-RAM/Word-RAM/PCM-RAM. Armed by a DTRG write with
 * DDS=4/5/7; fires on the next segacd_cdc_dma_update() call (once per CDD
 * tick from the frame loop). Mirrors pd_cd/cdc.c:358-406 cdc_dma_update() +
 * cdc.c:258-356 do_dma(), adapted to the harness's byte-swapped PRG-RAM
 * layout (off^1 stores — see segacd_bus.c:360).
 *
 * PRG-RAM (DDS=5) is the only destination the sub-BIOS is known to use for
 * game-program loads: the $0656 decompressor reads compressed blobs from
 * $15800/$13400/$19800 that the BIOS DMA'd in from disc. Without this path
 * the decompressor reads zeros and the game never boots past the BIOS.
 *
 * PCM-RAM (4) and Word-RAM (7) are armed but the transfer is a no-op until
 * a GA trace shows the BIOS relying on them. */
void segacd_cdc_dma_update(void)
{
    if (!CDC.dma_w) return;

    scd_dbg_dma_fire++;

    unsigned int dma_addr = ((unsigned int)SCD.s68k_regs[0x0a] << 8)
                          |  (unsigned int)SCD.s68k_regs[0x0b];
    unsigned int src_addr = CDC.dac & (CDC_RING_SIZE - 1);
    unsigned int bytes_in = (unsigned int)CDC.dbc + 1;

    if (CDC.dma_w == 5 && bytes_in && bytes_in <= SEGACD_PRG_RAM_SIZE) {
        /* PRG-RAM: dst = dma_addr << 3 (the LC89510 shifts the 16-bit DMA
         * pointer left by 3 to produce a 19-bit PRG-RAM byte address). */
        unsigned int dst_addr = (dma_addr << 3) & (SEGACD_PRG_RAM_SIZE - 1);
        for (unsigned int i = 0; i < bytes_in; i += 2) {
            unsigned int s = (src_addr + i) & (CDC_RING_SIZE - 1);
            unsigned int d = (dst_addr + i) & (SEGACD_PRG_RAM_SIZE - 1);
            /* CDC ring is big-endian; PRG-RAM byte writes use (off^1) so
             * subsequent sub-68K word reads reconstruct the same big-endian
             * word the disc had. */
            sub_prg_paged_write8(d ^ 1, CD.cdc_ram[s]);
            sub_prg_paged_write8((d + 1) ^ 1, CD.cdc_ram[(s + 1) & (CDC_RING_SIZE - 1)]);
        }
    }
    /* TODO: DMA types 4 (PCM-RAM, dst = (dma_addr<<2)&0xffc into pcm_ram)
     * and 7 (Word-RAM, 2M/1M bank dependent) if a trace needs them. */

    /* Post-DMA state — mirrors pd_cd/cdc.c:380-406. */
    CDC.dac = (uint16_t)(CDC.dac + bytes_in);
    dma_addr += bytes_in >> 3;
    SCD.s68k_regs[0x0a] = (uint8_t)(dma_addr >> 8);
    SCD.s68k_regs[0x0b] = (uint8_t)(dma_addr & 0xff);
    CDC.dbc = 0xffff;
    CDC.ifstat |= (uint8_t)(CDC_IFSTAT_DTBSY | CDC_IFSTAT_DTEN);   /* idle: busy+disabled */
    SCD.s68k_regs[0x04] = (uint8_t)((SCD.s68k_regs[0x04] & 0x07) | 0x80);  /* EDT, DSR cleared */

    /* DTEI end-of-transfer IRQ (level 5) — active-low bit, so clearing it
     * signals the interrupt condition. */
    CDC.ifstat &= (uint8_t)~CDC_IFSTAT_DTEI;
    if (CDC.ifctrl & CDC_IFCTRL_DTEIEN)
        SCD.cdc_int_pending = 1;

    CDC.dma_w = 0;
}

/* Decoder half of the CDC: called once per CDD tick (75 Hz) from
 * segacd_cd_update() with the sector's 4-byte header and (for data tracks)
 * its 2048 bytes of user data. When CTRL0's DECEN bit is set, captures the
 * header/marks data valid, and if WRRQ is also set, writes header+data into
 * the ring at the PT/WA cursor (wrapping) and arms the level-5 DECI
 * interrupt. Mirrors cdc_decoder_update() (cdc.c:408-472). */
static void cdc_decoder_update(const uint8_t header[4], const uint8_t *sector_data)
{
    if (!(CDC.ctrl0 & CDC_CTRL0_DECEN)) return;
#ifdef SEGACD_GA_TRACE
    { extern uint32_t scd_dbg_dec_calls; scd_dbg_dec_calls++; }
#endif

    memcpy(CDC.head, header, 4);
    CDC.stat3 = 0x00;                 /* !VALST: data is valid */
    CDC.stat0 = CDC_CTRL0_DECEN;      /* CRCOK */
    CDC.ifstat &= (uint8_t)~CDC_IFSTAT_DECI;

    if (CDC.ifctrl & CDC_IFCTRL_DECIEN)
        SCD.cdc_int_pending = 1;

    if ((CDC.ctrl0 & CDC_CTRL0_WRRQ) && sector_data) {
#ifdef SEGACD_GA_TRACE
        { extern uint32_t scd_dbg_dec_wrrq; scd_dbg_dec_wrrq++; }
#endif
        CDC.pt = (uint16_t)(CDC.pt + CD_SECTOR_RAW);
        CDC.wa = (uint16_t)(CDC.wa + CD_SECTOR_RAW);
        unsigned int offset = CDC.pt & (CDC_RING_SIZE - 1);
        memcpy(CD.cdc_ram + offset, header, 4);

        unsigned int room = CDC_RING_SIZE - (offset + 4);
        if (CD_SECTOR_DATA <= room) {
            memcpy(CD.cdc_ram + offset + 4, sector_data, CD_SECTOR_DATA);
        } else {
            memcpy(CD.cdc_ram + offset + 4, sector_data, room);
            memcpy(CD.cdc_ram, sector_data + room, CD_SECTOR_DATA - room);
        }
    }
}

/* Advance the drive mechanics: while playing, pull the next data sector,
 * hand it to the CDC decoder, and step the head, crossing track boundaries
 * and stopping at the end of the disc — mirrors pd_cd/cdd.c:723-793
 * `cdd_update()` (minus its CD-DA fader/scan handling, which we don't model
 * yet). Called once per CDD tick (75 Hz) from the frame loop, alongside
 * segacd_cdd_process().
 *
 * On real hardware the CDC reads data autonomously after a Seek, regardless
 * of whether CDD is in PLAY mode. The BIOS sends Seek (0x04) — never Play
 * (0x03) — for program/data reads, so we must feed sectors during READY too.
 * Without this, the BIOS can't load the game program and mode 0x10 is
 * unreachable (L5cdc=4 interrupts in 1500 frames = zero data delivery). */
void segacd_cd_update(void)
{
    /* Feed during PLAY (audio/streaming), READY (post-Seek data read), or
     * STOP (our TOC-reading state — see segacd_cdd_process latency expiry).
     * The sub-BIOS sends Seek during TOC reading and expects sectors. */
    if (!CD.opened || (CD.status != CDD_PLAY && CD.status != CDD_READY && CD.status != CDD_STOP)) return;
    scd_cdupd_pass++;

    /* Feed the CDC decoder on EVERY play tick, mirroring pd_cd/cdd.c
     * cdd_update() — the head walks up to the target from 3 sectors behind, and
     * the decoder must present a HEAD (MSF) for those pre-target/pregap sectors
     * too (cur_lba < 0), because the sub-BIOS's CDC position gate examines the
     * decoded HEAD to decide when to enable buffer-write (WRRQ). Only real
     * data-track sectors (cur_lba >= 0, non-audio) carry payload; pregap
     * sectors get the correct HEAD with a zeroed data buffer (their payload is
     * never buffered — WRRQ only turns on once the head is 1-4 sectors from the
     * wanted sector). Gating the decode on a successful read_sector() (the old
     * behavior) skipped the pregap entirely and the gate never fired. */
    static uint8_t sector_buf[CD_SECTOR_DATA];
    cd_track_t *t = (CD.cur_lba >= 0) ? track_at_lba((uint32_t)CD.cur_lba) : NULL;
    int on_data = (CD.cur_lba < 0) || (t && !t->is_audio);

    if (on_data) {
        scd_cdupd_feed++;
        int have_data = 0;
        if (CD.cur_lba >= 0 && t && !t->is_audio)
            have_data = (read_sector((uint32_t)CD.cur_lba, sector_buf, CD_SECTOR_DATA) == 0);
        if (!have_data)
            memset(sector_buf, 0, CD_SECTOR_DATA);   /* pregap / unreadable: HEAD only */

        int32_t msf = CD.cur_lba + 150;              /* >= 0 for cur_lba >= -150 */
        uint8_t header[4];
        header[0] = bcd8((int)((msf / 75) / 60));
        header[1] = bcd8((int)((msf / 75) % 60));
        header[2] = bcd8((int)(msf % 75));
        header[3] = 0x01;   /* CD-ROM Mode 1 */
#ifdef SEGACD_GA_TRACE
        if (have_data) { extern uint32_t scd_dbg_cdupd_read; scd_dbg_cdupd_read++; }
        { extern void scd_dbg_log_hdr(int32_t lba, const uint8_t *h, const uint8_t *data);
          scd_dbg_log_hdr(CD.cur_lba, header, sector_buf); }
#endif
        cdc_decoder_update(header, sector_buf);
    }
    /* TODO(ph4): CD-DA audio tracks -> stream to mixer, not the CDC ring. */

    CD.cur_lba++;

    /* Track-cross / end-of-disc only applies once the head is on a real track
     * (>= 0) — never during the pre-target pregap walk. */
    if (CD.cur_lba >= 0) {
        cd_track_t *ct = track_at_lba((uint32_t)CD.cur_lba);
        if (!ct || (uint32_t)CD.cur_lba >= ct->start_lba + ct->length_lba) {
            if (CD.index + 1 < CD.num_tracks) {
                CD.index++;
                CD.cur_lba = (int32_t)CD.tracks[CD.index].start_lba;
            } else {
                /* End of disc: wrap back to track 0 instead of transitioning
                 * to CDD_END. CDD_END (0x0C) does NOT set $583A in the sub-BIOS
                 * level-2 ISR jump table (only statuses 1,5,6,8,9,D,E,F set it),
                 * which blocks the level-4 ISR body ($2532 subchannel Q
                 * processing) and deadlocks the sub-BIOS CDD command loop.
                 * Wrapping keeps the status at READY/PLAY so $583A can be set
                 * through the normal status transition path. */
                CD.index = 0;
                CD.cur_lba = (int32_t)CD.tracks[0].start_lba;
            }
        }
    }
}

#ifdef SEGACD_GA_TRACE
/* Boot-stall visibility accessor: expose the file-static CDC/CD fields the
 * host boot harness prints, without making the whole struct external. */
uint16_t segacd_cdc_ctrl_dbg(int which)
{
    switch (which) {
    case 0: return CDC.ctrl0;
    case 1: return CDC.ctrl1;
    case 2: return CDC.ifctrl;
    case 3: return CDC.ifstat;
    case 4: return (uint16_t)CD.status;
    case 5: return (uint16_t)CD.cur_lba;
    default: return 0;
    }
}
#endif

#ifdef SEGACD_GA_TRACE
/* Boot-stall: log the first data sectors we feed the decoder — LBA, the 4-byte
 * MSF/mode header the sub reads from HEAD0-3, and whether the 2048 bytes look
 * like the Sega CD boot descriptor ("SEGADISCSYSTEM"). */
static int scd_dbg_hdr_n;
void scd_dbg_log_hdr(int32_t lba, const uint8_t *h, const uint8_t *data)
{
    if (scd_dbg_hdr_n >= 24) return;
    char sig[17]; memcpy(sig, data, 16); sig[16]=0;
    for (int i=0;i<16;i++) if (sig[i]<32||sig[i]>126) sig[i]='.';
    printf("[boot] sector LBA=%d HEAD=%02x %02x %02x %02x  data[0..15]='%s'\n",
           lba, h[0],h[1],h[2],h[3], sig);
    scd_dbg_hdr_n++;
}
#endif
