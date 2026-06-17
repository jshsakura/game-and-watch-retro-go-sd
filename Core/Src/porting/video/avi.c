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
            fseek(a->f, data + 32, SEEK_SET);
            a->width  = (int)rd_u32(a->f);                    // +32
            a->height = (int)rd_u32(a->f);                    // +36
            return;
        }
        pos = data + sz + (sz & 1);                           // skip (LISTs too)
    }
}

bool avi_open(avi_t *a, const char *path)
{
    memset(a, 0, sizeof *a);
    a->f = fopen(path, "rb");
    if (!a->f) return false;

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
                a->movi_pos = inner;
                a->movi_end = next < a->file_end ? next : a->file_end;
                if (a->width <= 0)  a->width = 320;            // sane fallbacks
                if (a->height <= 0) a->height = 240;
                if (a->usec_per_frame <= 0) a->usec_per_frame = 41667;
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
            *size = (long)sz; fseek(a->f, data, SEEK_SET); return AVI_VIDEO;
        }
        if (t0 == 'w' && t1 == 'b') {
            *size = (long)sz; fseek(a->f, data, SEEK_SET); return AVI_AUDIO;
        }
        // JUNK / 'ix..' index / unknown -> skip (cursor already advanced)
    }
    return AVI_END;
}

void avi_close(avi_t *a)
{
    if (a->f) { fclose(a->f); a->f = NULL; }
}
