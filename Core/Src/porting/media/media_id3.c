// ID3v2 metadata reader — see media_id3.h.
//
// Reads frame headers straight off disk (fseek/fread) so no large tag buffer is
// needed; only one frame's data is held in RAM at a time. Text is decoded from
// the four ID3 encodings (latin1 / UTF-16+BOM / UTF-16BE / UTF-8) to UTF-8.

#include "media_id3.h"
#include <stdio.h>
#include <string.h>

// Largest single frame we buffer (lyrics can be long; cover art is read
// separately by id3_read_cover and never lands here).
static uint8_t g_frame[ID3_LYRICS_MAX + 16];

// --- UTF-8 encoding ---------------------------------------------------------

// Append one Unicode code point as UTF-8. Returns bytes written (0 if no room).
static int put_utf8(uint32_t cp, char *out, int cap)
{
    if (cp < 0x80) {
        if (cap < 1) return 0;
        out[0] = (char)cp; return 1;
    }
    if (cp < 0x800) {
        if (cap < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    if (cap < 4) return 0;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

// Decode `n` bytes of `s` in ID3 encoding `enc` into `out` (UTF-8, NUL-term).
// enc: 0=ISO-8859-1, 1=UTF-16 w/ BOM, 2=UTF-16BE, 3=UTF-8.
static void to_utf8(int enc, const uint8_t *s, int n, char *out, int cap)
{
    int o = 0;
    cap -= 1;                                   // reserve the NUL
    if (cap < 0) { if (cap == -1) out[0] = '\0'; return; }

    if (enc == 3) {                             // already UTF-8
        for (int i = 0; i < n && o < cap; i++) {
            if (s[i] == 0) break;
            out[o++] = (char)s[i];
        }
    } else if (enc == 0) {                       // latin1
        for (int i = 0; i < n; i++) {
            if (s[i] == 0) break;
            int w = put_utf8(s[i], out + o, cap - o);
            if (!w) break;
            o += w;
        }
    } else {                                     // UTF-16 (1 BOM, 2 BE)
        int i = 0;
        bool be = (enc == 2);
        if (enc == 1 && n >= 2) {
            if (s[0] == 0xFF && s[1] == 0xFE) { be = false; i = 2; }
            else if (s[0] == 0xFE && s[1] == 0xFF) { be = true; i = 2; }
        }
        for (; i + 1 < n; i += 2) {
            uint32_t u = be ? (((uint32_t)s[i] << 8) | s[i + 1])
                            : (((uint32_t)s[i + 1] << 8) | s[i]);
            if (u == 0) break;                   // embedded NUL terminates
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {   // surrogate pair
                uint32_t lo = be ? (((uint32_t)s[i + 2] << 8) | s[i + 3])
                                 : (((uint32_t)s[i + 3] << 8) | s[i + 2]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            int w = put_utf8(u, out + o, cap - o);
            if (!w) break;
            o += w;
        }
    }
    out[o] = '\0';
}

// --- frame iteration --------------------------------------------------------

static uint32_t synchsafe(const uint8_t *b)
{
    return ((uint32_t)(b[0] & 0x7f) << 21) | ((uint32_t)(b[1] & 0x7f) << 14) |
           ((uint32_t)(b[2] & 0x7f) << 7)  |  (uint32_t)(b[3] & 0x7f);
}

// Copy a text frame (data[0]=encoding, rest=text) into a UTF-8 field.
static void store_text(const uint8_t *data, int n, char *out, int cap)
{
    if (n < 1) { out[0] = '\0'; return; }
    to_utf8(data[0], data + 1, n - 1, out, cap);
}

// Copy a COMM/USLT frame (enc, 3-byte lang, NUL-terminated desc, then text).
static void store_lang_text(const uint8_t *data, int n, char *out, int cap)
{
    if (n < 5) { out[0] = '\0'; return; }
    int enc = data[0];
    int p = 4;                                   // skip enc + 3-byte language
    if (enc == 1 || enc == 2) {                  // UTF-16 desc: 2-byte NUL
        while (p + 1 < n && !(data[p] == 0 && data[p + 1] == 0)) p += 2;
        p += 2;
    } else {                                     // latin1/utf8 desc: 1-byte NUL
        while (p < n && data[p] != 0) p++;
        p += 1;
    }
    if (p > n) p = n;
    to_utf8(enc, data + p, n - p, out, cap);
}

// Match a 4-char (v2.3/2.4) or 3-char (v2.2) frame id.
static bool fid(const uint8_t *h, const char *id4, const char *id3, int idlen)
{
    return memcmp(h, idlen == 4 ? id4 : id3, idlen) == 0;
}

void id3_read_tags(const char *path, media_tags_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return;

    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) { fclose(f); return; }

    int ver = hdr[3];                            // 2, 3 or 4
    int idlen = (ver == 2) ? 3 : 4;
    int fhlen = (ver == 2) ? 6 : 10;
    uint32_t tagsize = synchsafe(hdr + 6);
    long tag_end = 10 + (long)tagsize;
    long pos = 10;

    if (ver >= 3 && (hdr[5] & 0x40)) {           // skip extended header
        uint8_t eh[4];
        if (fseek(f, pos, SEEK_SET) == 0 && fread(eh, 1, 4, f) == 4) {
            uint32_t esz = (ver == 4) ? synchsafe(eh)
                : (((uint32_t)eh[0] << 24) | ((uint32_t)eh[1] << 16) |
                   ((uint32_t)eh[2] << 8) | eh[3]);
            pos += (ver == 4) ? (long)esz : (long)esz + 4;
        }
    }

    out->found = true;

    while (pos + fhlen <= tag_end) {
        if (fseek(f, pos, SEEK_SET) != 0) break;
        uint8_t fh[10];
        if (fread(fh, 1, fhlen, f) != (size_t)fhlen) break;
        if (fh[0] == 0) break;                   // padding

        uint32_t fsize;
        if (ver == 2)
            fsize = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
        else if (ver == 4)
            fsize = synchsafe(fh + 4);
        else
            fsize = ((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) |
                    ((uint32_t)fh[6] << 8) | fh[7];

        long fdata = pos + fhlen;
        pos = fdata + (long)fsize;               // advance to next frame

        if (fsize == 0 || fsize > sizeof(g_frame)) continue;

        int n = (int)fread(g_frame, 1, fsize, f);
        if (n <= 0) continue;

        if      (fid(fh, "TIT2", "TT2", idlen)) store_text(g_frame, n, out->title, ID3_FIELD);
        else if (fid(fh, "TPE1", "TP1", idlen)) store_text(g_frame, n, out->artist, ID3_FIELD);
        else if (fid(fh, "TALB", "TAL", idlen)) store_text(g_frame, n, out->album, ID3_FIELD);
        else if (fid(fh, "TPE2", "TP2", idlen)) store_text(g_frame, n, out->album_artist, ID3_FIELD);
        else if (fid(fh, "TCOM", "TCM", idlen)) store_text(g_frame, n, out->composer, ID3_FIELD);
        else if (fid(fh, "TCON", "TCO", idlen)) store_text(g_frame, n, out->genre, ID3_FIELD);
        else if (fid(fh, "TRCK", "TRK", idlen)) store_text(g_frame, n, out->track, sizeof(out->track));
        else if (fid(fh, "TYER", "TYE", idlen)) { if (!out->year[0]) store_text(g_frame, n, out->year, sizeof(out->year)); }
        else if (fid(fh, "TDRC", "TDA", idlen)) store_text(g_frame, n, out->year, sizeof(out->year));
        else if (fid(fh, "COMM", "COM", idlen)) store_lang_text(g_frame, n, out->comment, ID3_FIELD);
        else if (fid(fh, "USLT", "ULT", idlen)) {
            store_lang_text(g_frame, n, out->lyrics, ID3_LYRICS_MAX);
            out->has_lyrics = (out->lyrics[0] != '\0');
        }
    }
    fclose(f);
}

// --- cover art --------------------------------------------------------------

int id3_read_cover(const char *path, uint8_t *dst, int cap, bool *is_png)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) { fclose(f); return 0; }
    int ver = hdr[3];
    int idlen = (ver == 2) ? 3 : 4;
    int fhlen = (ver == 2) ? 6 : 10;
    long tag_end = 10 + (long)synchsafe(hdr + 6);
    long pos = 10;
    int img_n = 0;

    while (pos + fhlen <= tag_end) {
        if (fseek(f, pos, SEEK_SET) != 0) break;
        uint8_t fh[10];
        if (fread(fh, 1, fhlen, f) != (size_t)fhlen) break;
        if (fh[0] == 0) break;

        uint32_t fsize;
        if (ver == 2)
            fsize = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
        else if (ver == 4)
            fsize = synchsafe(fh + 4);
        else
            fsize = ((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) |
                    ((uint32_t)fh[6] << 8) | fh[7];
        long fdata = pos + fhlen;

        bool is_pic = fid(fh, "APIC", "PIC", idlen);
        if (is_pic && fsize > 10) {
            fseek(f, fdata, SEEK_SET);
            long consumed = 0;
            int enc = fgetc(f); consumed++;                  // text encoding
            if (idlen == 3) {                                // v2.2: 3-byte format
                fgetc(f); fgetc(f); fgetc(f); consumed += 3;
            } else {                                         // v2.3/4: MIME string
                while (consumed < (long)fsize && fgetc(f) != 0) consumed++;
                consumed++;
            }
            (void)fgetc(f); consumed++;                      // picture type
            if (enc == 1 || enc == 2) {                      // UTF-16 desc: 2-byte NUL
                int z = 0;
                while (consumed < (long)fsize) {
                    int c = fgetc(f); consumed++;
                    if (c == 0) { if (++z == 2) break; } else z = 0;
                }
            } else {                                         // latin1/utf8 desc
                while (consumed < (long)fsize && fgetc(f) != 0) consumed++;
                consumed++;
            }
            long isz = (long)fsize - consumed;
            if (isz > 8) {
                long lim = isz < cap ? isz : cap;
                img_n = (int)fread(dst, 1, (size_t)lim, f);
                if (img_n > 8) {
                    *is_png = (dst[0] == 0x89 && dst[1] == 'P' && dst[2] == 'N' && dst[3] == 'G');
                } else {
                    img_n = 0;
                }
            }
            break;                                           // first picture only
        }
        pos = fdata + (long)fsize;
    }
    fclose(f);
    return img_n;
}

// --- sidecar lyrics ---------------------------------------------------------

bool id3_read_lrc(const char *mp3path, char *out, int cap)
{
    char lrc[260];
    size_t ln = strlen(mp3path);
    if (ln + 1 >= sizeof(lrc)) return false;
    memcpy(lrc, mp3path, ln + 1);
    char *dot = strrchr(lrc, '.');
    if (!dot) return false;
    if ((size_t)(dot - lrc) + 5 >= sizeof(lrc)) return false;
    memcpy(dot, ".lrc", 5);

    FILE *f = fopen(lrc, "rb");
    if (!f) return false;
    int n = (int)fread(out, 1, cap - 1, f);
    fclose(f);
    if (n <= 0) return false;
    out[n] = '\0';
    return true;
}
