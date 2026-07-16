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

typedef struct {
    uint32_t start_lba;
    uint32_t length_lba;
    uint32_t file_offset;
    uint16_t sector_size;    /* 2048 or 2352 */
    uint8_t  is_audio;
    char     bin_path[256];
} cd_track_t;

typedef struct {
    cd_track_t tracks[CD_MAX_TRACKS];
    int        num_tracks;
    uint32_t   total_lba;

    FILE      *fh;                 /* persistent handle to the currently-open bin */
    char       fh_path[256];       /* which bin fh points at (avoid reopen) */
    uint32_t   fh_pos;             /* cached byte offset (skip lseek if sequential) */

    uint32_t   cur_lba;            /* CDD head position */
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
    uint32_t cur_file_base_lba = 0;   /* absolute LBA at offset 0 of cur_bin */
    uint32_t running_lba = 0;         /* total sectors across files seen so far */
    int      ti = -1;
    char     line[512];
    CD.num_tracks = 0;

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
        } else if (!strncmp(p, "TRACK", 5) && CD.num_tracks < CD_MAX_TRACKS) {
            int num = 0; char mode[32] = {0};
            if (sscanf(p, "TRACK %d %31s", &num, mode) != 2) continue;
            ti = CD.num_tracks++;
            cd_track_t *t = &CD.tracks[ti];
            memset(t, 0, sizeof(*t));
            snprintf(t->bin_path, sizeof(t->bin_path), "%s", cur_bin);
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

    if (CD.fh == NULL || strcmp(CD.fh_path, t->bin_path) != 0) {
        if (CD.fh) fclose(CD.fh);
        CD.fh = fopen(t->bin_path, "rb");
        if (!CD.fh) return -1;
        snprintf(CD.fh_path, sizeof(CD.fh_path), "%s", t->bin_path);
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

int segacd_bram_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { memset(SCD.bram, 0, sizeof(SCD.bram)); return -1; }
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
#endif

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

    case 0x02:  /* Read TOC — c[1] selects which field (Q-channel infos) */
        if (CD.status == CDD_NODISC)
            CD.status = CD.opened ? CDD_STOP : CDD_NODISC;
        switch (c[1] & 0x0f) {
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
        case 0x05: {  /* track start time; track number requested in c[2..3] */
            int trk = c[2]*10 + c[3];
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
            s[0] = (uint8_t)CD.status; s[1] = c[1] & 0x0f;
            s[2] = 0; s[3] = 0; s[4] = 0; s[5] = 0; s[6] = 0; s[7] = 0; s[8] = 0;
            break;
        }
        break;

    case 0x03:    /* Play from LBA */
    case 0x04: {  /* Seek to LBA — same addressing, only the settle state differs */
        int lba = ((c[2]*10+c[3])*60 + (c[4]*10+c[5]))*75 + (c[6]*10+c[7]) - 150;
        if (lba < 0) lba = 0;
        CD.index        = track_index_at_lba((uint32_t)lba);
        CD.cur_lba      = (uint32_t)(lba > 3 ? lba - 3 : 0);  /* playback starts a few blocks early, pd_cd/cdd.c:1059 */
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
        SCD.s68k_regs[0x38] = (uint8_t)CD.status;
        cdd_status_checksum();
        SCD.cdd_int_pending = 1;   /* periodic CDD IRQ (level 4) drives the sub-BIOS */
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

/* Advance the drive mechanics: while playing, pull the next data sector into
 * the CDC ring and step the head, crossing track boundaries and stopping at
 * the end of the disc — mirrors pd_cd/cdd.c:723-793 `cdd_update()` (minus its
 * CD-DA fader/scan handling, which we don't model yet). Called once per CDD
 * tick (75 Hz) from the frame loop, alongside segacd_cdd_process(). */
void segacd_cd_update(void)
{
    if (!CD.opened || CD.status != CDD_PLAY) return;

    cd_track_t *t = track_at_lba(CD.cur_lba);
    if (t && !t->is_audio)
        read_sector(CD.cur_lba, CD.cdc_ram, CD_SECTOR_DATA);
    /* TODO(ph4): CD-DA audio tracks -> stream to mixer, not the CDC ring. */

    CD.cur_lba++;

    if (!t || CD.cur_lba >= t->start_lba + t->length_lba) {
        /* crossed into the next track, or ran off a track we couldn't find */
        if (CD.index + 1 < CD.num_tracks) {
            CD.index++;
            CD.cur_lba = CD.tracks[CD.index].start_lba;
        } else {
            CD.status = CDD_END;
        }
    }
}
