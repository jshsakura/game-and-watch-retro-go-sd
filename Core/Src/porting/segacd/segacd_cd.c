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

/* CDD status codes (subset; PicoDrive cdd.h). */
enum { CDD_STOP = 0, CDD_PLAY = 1, CDD_SEEK = 2, CDD_PAUSE = 4,
       CDD_READY = 9, CDD_TRAY = 0xE };

/* ---- cue/TOC parse (structure mirrors pce_cd.c) ---- */

static int parse_cue(const char *cue_path)
{
    FILE *cue = fopen(cue_path, "rb");
    if (!cue) return -1;

    char line[512], cur_bin[256] = {0};
    uint32_t base_lba = 0, running_lba = 0;
    CD.num_tracks = 0;

    while (fgets(line, sizeof(line), cue)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (!strncmp(p, "FILE", 4)) {
            char *q = strchr(p, '"'), *r = q ? strchr(q + 1, '"') : NULL;
            if (q && r) { size_t n = (size_t)(r - q - 1);
                if (n >= sizeof(cur_bin)) n = sizeof(cur_bin) - 1;
                memcpy(cur_bin, q + 1, n); cur_bin[n] = 0; }
            base_lba = running_lba;
            /* running_lba advanced when we know the file's sector count below */
        } else if (!strncmp(p, "TRACK", 5) && CD.num_tracks < CD_MAX_TRACKS) {
            cd_track_t *t = &CD.tracks[CD.num_tracks++];
            memset(t, 0, sizeof(*t));
            snprintf(t->bin_path, sizeof(t->bin_path), "%s", cur_bin);
            t->is_audio     = (strstr(p, "AUDIO") != NULL);
            t->sector_size  = strstr(p, "/2048") ? CD_SECTOR_DATA : CD_SECTOR_RAW;
        } else if (!strncmp(p, "INDEX", 5) && CD.num_tracks > 0) {
            int mm = 0, ss = 0, ff = 0;
            if (sscanf(p, "INDEX %*d %d:%d:%d", &mm, &ss, &ff) == 3) {
                uint32_t frames = (uint32_t)((mm * 60 + ss) * 75 + ff);
                cd_track_t *t = &CD.tracks[CD.num_tracks - 1];
                t->start_lba   = base_lba + frames;
                t->file_offset = frames * t->sector_size;
            }
        }
    }
    fclose(cue);

    /* total length: last track to EOF of its bin (best-effort) */
    for (int i = 0; i < CD.num_tracks; i++) {
        uint32_t end = (i + 1 < CD.num_tracks) ? CD.tracks[i + 1].start_lba : running_lba;
        CD.tracks[i].length_lba = end > CD.tracks[i].start_lba ? end - CD.tracks[i].start_lba : 0;
        if (end > CD.total_lba) CD.total_lba = end;
    }
    return CD.num_tracks > 0 ? 0 : -1;
}

static cd_track_t *track_at_lba(uint32_t lba)
{
    for (int i = 0; i < CD.num_tracks; i++) {
        cd_track_t *t = &CD.tracks[i];
        if (lba >= t->start_lba && lba < t->start_lba + t->length_lba) return t;
    }
    return CD.num_tracks ? &CD.tracks[0] : NULL;
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
    CD.status = CDD_READY;
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

/* Process one CDD command from the gate-array command buffer and update status.
 * Command/status live in SCD.s68k_regs (CDD command $A12042.., status $A12038..).
 * TODO(ph3d): full 10-byte command decode + checksum + subcode. This handles the
 * load-bearing ones so the BIOS can seek and read. */
void segacd_cdd_process(void)
{
    uint8_t cmd = SCD.s68k_regs[0x42 & (SEGACD_GA_REGS - 1)];
    switch (cmd) {
    case 0x00:  /* status / no-op */
        break;
    case 0x02:  /* read TOC — report total length / track info */
        CD.status = CDD_READY;
        break;
    case 0x03:  /* play from LBA */
        CD.status = CDD_PLAY;
        break;
    case 0x04:  /* seek to LBA */
        CD.status = CDD_SEEK;
        break;
    case 0x06:  /* pause */
        CD.status = CDD_PAUSE;
        break;
    case 0x08:  /* resume */
        CD.status = CDD_PLAY;
        break;
    default:
        break;
    }
    SCD.s68k_regs[0x38 & (SEGACD_GA_REGS - 1)] = (uint8_t)CD.status;
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

/* Advance the drive: if reading, pull the next data sector into the CDC ring.
 * Called once per CDD tick (75 Hz) from the frame loop. */
void segacd_cd_update(void)
{
    if (!CD.opened) return;
    if (CD.status == CDD_PLAY) {
        cd_track_t *t = track_at_lba(CD.cur_lba);
        if (t && !t->is_audio) {
            read_sector(CD.cur_lba, CD.cdc_ram, CD_SECTOR_DATA);
            CD.cur_lba++;
        }
        /* TODO(ph4): CD-DA audio tracks -> stream to mixer, not the CDC ring. */
    }
}
