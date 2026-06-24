// AVI/RIFF demuxer — see avi.h.

#include "avi.h"
#include <string.h>

// --- little-endian readers -------------------------------------------------

static uint32_t rd_u32(FILE *f)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static bool rd_fourcc(FILE *f, char out[4])
{
    return fread(out, 1, 4, f) == 4;
}

#define FCC(p, s) (memcmp((p), (s), 4) == 0)

// --- header parse ----------------------------------------------------------

// Scan an 'hdrl' LIST (bounded by [pos, end)) for the 'avih' main header and
// pull the frame size + rate out of it. avih layout (after id+size):
//   +0  dwMicroSecPerFrame   +32 dwWidth   +36 dwHeight
static void parse_hdrl(avi_t *a, long pos, long end)
{
    while (pos + 8 <= end) {
        if (fseek(a->f, pos, SEEK_SET) != 0) return;
        char id[4];
        if (!rd_fourcc(a->f, id)) return;
        uint32_t sz = rd_u32(a->f);
        long data = ftell(a->f);
        if (FCC(id, "avih")) {
            a->usec_per_frame = (int)rd_u32(a->f);            // +0
            fseek(a->f, data + 16, SEEK_SET);
            a->total_frames = (int)rd_u32(a->f);              // +16 dwTotalFrames
            fseek(a->f, data + 32, SEEK_SET);
            a->width  = (int)rd_u32(a->f);                    // +32
            a->height = (int)rd_u32(a->f);                    // +36
            return;
        }
        pos = data + sz + (sz & 1);                           // skip (LISTs too)
    }
}

bool avi_open(avi_t *a, const char *path, void *rabuf, unsigned long rasize)
{
    memset(a, 0, sizeof *a);
    a->f = fopen(path, "rb");
    if (!a->f) return false;

    // Read-ahead: a big fully-buffered stdio buffer turns the demuxer's many tiny
    // chunk reads into a few large block reads — the win on a slow SD, where the
    // per-read overhead (not throughput) is what stutters playback. Must be set
    // before any I/O on the stream.
    if (rabuf && rasize) setvbuf(a->f, (char *)rabuf, _IOFBF, (size_t)rasize);

    if (fseek(a->f, 0, SEEK_END) != 0) goto fail;
    a->file_end = ftell(a->f);
    rewind(a->f);

    char cc[4];
    if (!rd_fourcc(a->f, cc) || !FCC(cc, "RIFF")) goto fail;
    (void)rd_u32(a->f);                                       // riff size
    if (!rd_fourcc(a->f, cc) || !FCC(cc, "AVI ")) goto fail;

    // Walk top-level chunks: parse hdrl (frame size/rate), then locate movi.
    long pos = ftell(a->f);
    while (pos + 8 <= a->file_end) {
        if (fseek(a->f, pos, SEEK_SET) != 0) goto fail;
        if (!rd_fourcc(a->f, cc)) goto fail;
        uint32_t sz = rd_u32(a->f);
        long body = ftell(a->f);                              // first byte after id+size
        long next = body + sz + (sz & 1);

        if (FCC(cc, "LIST")) {
            char lt[4];
            if (!rd_fourcc(a->f, lt)) goto fail;
            long inner = ftell(a->f);                         // first byte after list type
            if (FCC(lt, "hdrl")) {
                parse_hdrl(a, inner, next);
            } else if (FCC(lt, "movi")) {
                a->movi_start = inner;
                a->movi_pos = inner;
                a->movi_end = next < a->file_end ? next : a->file_end;
                if (a->width <= 0)  a->width = 320;            // sane fallbacks
                if (a->height <= 0) a->height = 240;
                if (a->usec_per_frame <= 0) a->usec_per_frame = 41667;
                a->ckpt_step = a->total_frames / AVI_CKPT_N;   // sparse seek index
                if (a->ckpt_step < 1) a->ckpt_step = 1;
                a->ckpt[0] = a->movi_start;                    // frame 0 lives here
                fseek(a->f, a->movi_pos, SEEK_SET);
                return true;
            }
        }
        if (next <= pos) goto fail;                           // malformed: no progress
        pos = next;
    }

fail:
    if (a->f) { fclose(a->f); a->f = NULL; }
    return false;
}

avi_kind_t avi_next(avi_t *a, long *size)
{
    while (a->movi_pos + 8 <= a->movi_end) {
        if (fseek(a->f, a->movi_pos, SEEK_SET) != 0) break;
        char id[4];
        if (!rd_fourcc(a->f, id)) break;
        uint32_t sz = rd_u32(a->f);
        long data = ftell(a->f);

        if (FCC(id, "LIST")) {                                // 'rec ' grouping: descend
            a->movi_pos = data + 4;
            continue;
        }
        a->movi_pos = data + sz + (sz & 1);                   // advance past this chunk

        if (sz == 0) continue;                                // empty / dropped frame
        // chunk id = "NNxx": NN = stream index, xx = type ("dc"/"db" video, "wb" audio)
        char t0 = id[2], t1 = id[3];
        if (t0 == 'd' && (t1 == 'c' || t1 == 'b')) {
            *size = (long)sz; fseek(a->f, data, SEEK_SET);
            a->cur_frame++;
            if (a->ckpt_step > 0 && a->cur_frame % a->ckpt_step == 0) {   // record a seek checkpoint
                int kk = a->cur_frame / a->ckpt_step;
                if (kk > 0 && kk < AVI_CKPT_N) a->ckpt[kk] = a->movi_pos;
            }
            return AVI_VIDEO;
        }
        if (t0 == 'w' && t1 == 'b') {
            *size = (long)sz; fseek(a->f, data, SEEK_SET); return AVI_AUDIO;
        }
        // JUNK / 'ix..' index / unknown -> skip (cursor already advanced)
    }
    return AVI_END;
}

// Pre-fill the sparse seek checkpoints from the AVI 'idx1' index (the table of
// every chunk's offset/size that sits right after the movi list). This makes a
// big FORWARD jump into a not-yet-played region fast: seek lands on the nearest
// checkpoint and walks only a few frames, instead of stepping through thousands
// of chunks. Done lazily on the first seek so it never slows the initial open.
// Fully defensive — any inconsistency just leaves ckpt[] as recorded during
// playback, so seeking still works (only slower). idx1 offsets are stored either
// relative to the 'movi' fourcc or as file-absolute; the base is auto-detected.
static void avi_build_index(avi_t *a)
{
    a->indexed = true;                                    // try once, success or not
    if (a->total_frames <= 0 || a->ckpt_step <= 0) return;

    // walk top-level chunks from movi_end to find 'idx1'
    long pos = a->movi_end, idx_pos = 0;
    uint32_t idx_sz = 0;
    while (pos + 8 <= a->file_end) {
        if (fseek(a->f, pos, SEEK_SET) != 0) return;
        char id[4];
        if (!rd_fourcc(a->f, id)) return;
        uint32_t sz = rd_u32(a->f);
        long body = ftell(a->f);
        if (FCC(id, "idx1")) { idx_pos = body; idx_sz = sz; break; }
        long next = body + sz + (sz & 1);
        if (next <= pos) return;
        pos = next;
    }
    if (!idx_pos || idx_sz < 16) return;

    // detect the offset base from the first entry: id[4] flags[4] offset[4] size[4]
    uint8_t e[16];
    if (fseek(a->f, idx_pos, SEEK_SET) != 0) return;
    if (fread(e, 1, 16, a->f) != 16) return;
    uint32_t off0 = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                    ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
    long base = 0;
    bool have_base = false;
    long cand[2] = { a->movi_start - 4, 0 };              // movi-relative, then file-absolute
    for (int t = 0; t < 2 && !have_base; t++) {
        char cid[4];
        if (fseek(a->f, cand[t] + (long)off0, SEEK_SET) == 0 && rd_fourcc(a->f, cid) &&
            cid[0] == (char)e[0] && cid[1] == (char)e[1] &&
            cid[2] == (char)e[2] && cid[3] == (char)e[3]) {
            base = cand[t]; have_base = true;
        }
    }
    if (!have_base) return;                               // unknown layout -> keep playback ckpts

    // stream the index sequentially, recording a checkpoint at every ckpt_step-th
    // video frame (ckpt[k] points at the chunk id of the (k*ckpt_step+1)-th frame)
    if (fseek(a->f, idx_pos, SEEK_SET) != 0) return;
    long n = idx_sz / 16;
    int vf = 0;                                           // 1-based video-frame index
    for (long i = 0; i < n; i++) {
        if (fread(e, 1, 16, a->f) != 16) break;
        if (e[2] != 'd' || (e[3] != 'c' && e[3] != 'b')) continue;   // video chunks only
        vf++;
        if (vf >= 2 && (vf - 1) % a->ckpt_step == 0) {
            int k = (vf - 1) / a->ckpt_step;
            if (k > 0 && k < AVI_CKPT_N && a->ckpt[k] == 0) {
                uint32_t off = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                               ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                a->ckpt[k] = base + (long)off;
            }
        }
    }
}

void avi_seek_frame(avi_t *a, int frame)
{
    if (frame < 0) frame = 0;
    if (a->total_frames > 0 && frame >= a->total_frames) frame = a->total_frames - 1;

    if (!a->indexed) avi_build_index(a);   // lazy: fill ckpt[] across the whole file once

    // Jump to the nearest checkpoint at/just-before the target whenever it is closer
    // to the target than where we are now — so both a big forward jump and a backward
    // seek land near the goal and walk only the short remainder (never the whole clip).
    int k = a->ckpt_step > 0 ? frame / a->ckpt_step : 0;
    if (k >= AVI_CKPT_N) k = AVI_CKPT_N - 1;
    while (k > 0 && a->ckpt[k] == 0) k--;                 // nearest recorded checkpoint
    int  ck_frame = k > 0 ? k * a->ckpt_step : 0;
    long ck_pos   = k > 0 ? a->ckpt[k] : a->movi_start;
    if (frame < a->cur_frame || ck_frame > a->cur_frame) {
        a->cur_frame = ck_frame;
        a->movi_pos  = ck_pos;
    }
    long sz;
    while (a->cur_frame < frame) {
        if (avi_next(a, &sz) == AVI_END) break;   // avi_next advances cur_frame on video
    }
}

void avi_close(avi_t *a)
{
    if (a->f) { fclose(a->f); a->f = NULL; }
}
