/*
 * progjpeg.c - Minimal progressive-JPEG DC-scan decoder. See progjpeg.h.
 *
 * Design notes:
 *  - A thin marker parser walks SOI/APPn/DQT/SOF2/DHT/DRI/SOS.
 *  - Only DC Huffman tables are stored (AC tables are not needed for the DC
 *    scan); AC table definitions are parsed/skipped so the stream stays
 *    aligned.
 *  - The DC scan is entropy-decoded with a bit reader that handles byte
 *    stuffing (0xFF00 -> 0xFF) and restart markers (RSTn).
 *  - Each block: dc_diff = extend(receive(magnitude(huff_decode_dc))),
 *    predictor += dc_diff, coeff = predictor << Al. The reconstructed sample
 *    is clamp(((coeff * quant[0]) >> 3) + 128) -- i.e. the 8x8 block average.
 *  - Interleaved MCU traversal uses each component's Hi x Vi sampling factors.
 *    Chroma planes are upsampled (nearest) onto the luma block grid.
 *  - Non-DC scans before the DC scan are skipped by scanning their entropy
 *    data up to the next marker (which is then pushed back for the parser).
 */
#include "progjpeg.h"

/* ---- marker bytes ---- */
#define M_SOI  0xD8
#define M_EOI  0xD9
#define M_SOF0 0xC0  /* baseline    (unsupported) */
#define M_SOF1 0xC1  /* ext.seq.    (unsupported) */
#define M_SOF2 0xC2  /* progressive (supported)   */
#define M_DHT  0xC4
#define M_DQT  0xDB
#define M_DRI  0xDD
#define M_SOS  0xDA

#define MAX_COMPS 3

/* ---- internal state ---- */

typedef struct {
    uint8_t bits[17];   /* bits[1..16] = count of codes of each length */
    uint8_t vals[256];  /* symbol values, ordered by code length       */
    int     mincode[17];
    int     maxcode[17];/* -1 if no codes of that length */
    int     valptr[17];
    int     defined;
} HuffTable;

typedef struct {
    int id;        /* component identifier from SOF */
    int hi, vi;    /* sampling factors */
    int qt;        /* quant table index */
    int dc_tbl;    /* DC Huffman table selector (from SOS) */
    int pred;      /* DC predictor */
    int bw, bh;    /* plane size in blocks */
    uint8_t *plane;
} Comp;

typedef struct {
    PjSource *src;
    int       have_peek, peek;

    /* bit reader */
    uint32_t  bit_buf;
    int       bit_cnt;
    int       hit_marker;
    int       marker_val;

    uint16_t  qt[4][64];
    int       qt_defined[4];
    HuffTable dc_huff[4];

    int       prec, width, height, ncomp;
    Comp      comp[MAX_COMPS];
    int       restart_interval;

    int       mcu_w, mcu_h;
    int       hmax, vmax;

    uint8_t  *planes;   /* MAX_COMPS * PJ_MAX_BLOCKS_W * PJ_MAX_BLOCKS_H */
    uint8_t  *out;
    size_t    out_cap;
} Pj;

/* ---- raw byte input with single-byte lookahead ---- */

static int rd(Pj *p) {
    if (p->have_peek) { p->have_peek = 0; return p->peek; }
    return p->src->get(p->src->user);
}
static void unread(Pj *p, int b) { p->peek = b; p->have_peek = 1; }
static int rd_u16(Pj *p, int *out) {
    int a = rd(p), b = rd(p);
    if (a < 0 || b < 0) return 0;
    *out = (a << 8) | b;
    return 1;
}

/* ---- bit reader over entropy-coded data ---- */

static void bits_reset(Pj *p) {
    p->bit_buf = 0;
    p->bit_cnt = 0;
    p->hit_marker = 0;
    p->marker_val = 0;
}

/* Next entropy byte, transparently handling FF stuffing/markers. Returns 0
 * (and sets hit_marker/marker_val) when a real marker terminates the data. */
static int next_entropy_byte(Pj *p, int *out) {
    int b = rd(p);
    if (b < 0) return 0;
    if (b == 0xFF) {
        int b2 = rd(p);
        if (b2 < 0) return 0;
        if (b2 == 0x00) { *out = 0xFF; return 1; }
        p->hit_marker = 1; p->marker_val = b2; return 0;
    }
    *out = b;
    return 1;
}

static int get_bit(Pj *p) {
    if (p->bit_cnt == 0) {
        int byte;
        if (p->hit_marker) return 0;
        if (!next_entropy_byte(p, &byte)) return 0;
        p->bit_buf = (uint32_t)byte;
        p->bit_cnt = 8;
    }
    p->bit_cnt--;
    return (p->bit_buf >> p->bit_cnt) & 1;
}

static int get_bits(Pj *p, int n) {
    int v = 0;
    while (n-- > 0) v = (v << 1) | get_bit(p);
    return v;
}

/* ---- Huffman ---- */

static void huff_build(HuffTable *h) {
    int code = 0, k = 0, l;
    for (l = 1; l <= 16; l++) {
        if (h->bits[l] == 0) {
            h->maxcode[l] = -1;
        } else {
            h->valptr[l]  = k;
            h->mincode[l] = code;
            code += h->bits[l];
            h->maxcode[l] = code - 1;
            k += h->bits[l];
        }
        code <<= 1;
    }
    h->defined = 1;
}

static int huff_decode(Pj *p, HuffTable *h) {
    int l, code = 0;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | get_bit(p);
        if (p->hit_marker) return -1;
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l])
            return h->vals[h->valptr[l] + (code - h->mincode[l])];
    }
    return -1;
}

static int extend(int v, int s) {
    if (s == 0) return 0;
    if (v < (1 << (s - 1))) v += (int)((~0u) << s) + 1;
    return v;
}

static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ---- segment parsers ---- */

static int parse_dqt(Pj *p, int len) {
    len -= 2;
    while (len > 0) {
        int pq_tq = rd(p); len--;
        if (pq_tq < 0) return 0;
        int pq = pq_tq >> 4, tq = pq_tq & 0xF, i;
        if (tq > 3 || (pq != 0 && pq != 1)) return 0;
        for (i = 0; i < 64; i++) {
            int v;
            if (pq) { if (!rd_u16(p, &v)) return 0; len -= 2; }
            else    { v = rd(p); if (v < 0) return 0; len--; }
            p->qt[tq][i] = (uint16_t)v;
        }
        p->qt_defined[tq] = 1;
    }
    return 1;
}

static int parse_dht(Pj *p, int len) {
    len -= 2;
    while (len > 0) {
        int tc_th = rd(p); len--;
        if (tc_th < 0) return 0;
        int tc = tc_th >> 4, th = tc_th & 0xF, i, total = 0;
        uint8_t counts[17];
        if (th > 3) return 0;
        counts[0] = 0;
        for (i = 1; i <= 16; i++) {
            int c = rd(p); if (c < 0) return 0; len--;
            counts[i] = (uint8_t)c;
            total += c;
        }
        if (total > 256) return 0;
        if (tc == 0) {                       /* DC table: keep it */
            HuffTable *h = &p->dc_huff[th];
            for (i = 0; i <= 16; i++) h->bits[i] = counts[i];
            for (i = 0; i < total; i++) {
                int v = rd(p); if (v < 0) return 0; len--;
                h->vals[i] = (uint8_t)v;
            }
            huff_build(h);
        } else {                              /* AC table: skip bytes */
            for (i = 0; i < total; i++) { if (rd(p) < 0) return 0; len--; }
        }
    }
    return 1;
}

static int parse_sof2(Pj *p) {
    int i;
    p->prec = rd(p);
    if (!rd_u16(p, &p->height)) return 0;
    if (!rd_u16(p, &p->width))  return 0;
    p->ncomp = rd(p);
    if (p->prec != 8) return 0;
    if (p->width <= 0 || p->height <= 0) return 0;
    if (p->ncomp < 1 || p->ncomp > MAX_COMPS) return 0;
    p->hmax = p->vmax = 1;
    for (i = 0; i < p->ncomp; i++) {
        int id = rd(p), samp = rd(p), qt = rd(p);
        if (id < 0 || samp < 0 || qt < 0) return 0;
        p->comp[i].id = id;
        p->comp[i].hi = samp >> 4;
        p->comp[i].vi = samp & 0xF;
        p->comp[i].qt = qt;
        if (p->comp[i].hi < 1 || p->comp[i].hi > 4) return 0;
        if (p->comp[i].vi < 1 || p->comp[i].vi > 4) return 0;
        if (qt > 3) return 0;
        if (p->comp[i].hi > p->hmax) p->hmax = p->comp[i].hi;
        if (p->comp[i].vi > p->vmax) p->vmax = p->comp[i].vi;
    }
    p->mcu_w = (p->width  + 8 * p->hmax - 1) / (8 * p->hmax);
    p->mcu_h = (p->height + 8 * p->vmax - 1) / (8 * p->vmax);
    return 1;
}

static int comp_by_id(Pj *p, int id) {
    int i;
    for (i = 0; i < p->ncomp; i++) if (p->comp[i].id == id) return i;
    return -1;
}

/* ---- DC scan decode ---- */

static int decode_block(Pj *p, Comp *c, int bx, int by, int al) {
    HuffTable *h = &p->dc_huff[c->dc_tbl];
    int s = huff_decode(p, h);
    int diff, coeff, sample;
    if (s < 0) return 0;
    if (s > 16) return 0;
    diff = extend(get_bits(p, s), s);
    c->pred += diff;
    coeff = c->pred << al;                 /* point transform (Al) */
    sample = ((coeff * p->qt[c->qt][0]) >> 3) + 128; /* DC = block average */
    sample = clamp8(sample);
    if (bx >= 0 && bx < c->bw && by >= 0 && by < c->bh)
        c->plane[by * c->bw + bx] = (uint8_t)sample;
    return 1;
}

/* Skip to the next restart marker after a complete interval. Returns:
 *  1 = restart consumed, continue;  2 = scan ended (non-RST marker seen,
 *  pushed back);  0 = error/EOF. */
static int sync_restart(Pj *p) {
    if (!p->hit_marker) {                  /* we ran out before the marker? */
        int b;
        p->bit_cnt = 0;
        for (;;) {
            b = rd(p);
            if (b < 0) return 0;
            if (b != 0xFF) continue;
            do { b = rd(p); } while (b == 0xFF);
            if (b < 0) return 0;
            if (b >= 0xD0 && b <= 0xD7) break;       /* restart */
            unread(p, b); unread(p, 0xFF); return 2;  /* other marker */
        }
    } else if (p->marker_val < 0xD0 || p->marker_val > 0xD7) {
        unread(p, p->marker_val); unread(p, 0xFF);
        return 2;
    }
    return 1;
}

static int decode_dc_scan(Pj *p, int *scan_comp, int nscan, int al) {
    int total_mcu = p->mcu_w * p->mcu_h, mcu, i;

    bits_reset(p);
    for (i = 0; i < p->ncomp; i++) p->comp[i].pred = 0;

    for (mcu = 0; mcu < total_mcu; mcu++) {
        int mcx = mcu % p->mcu_w, mcy = mcu / p->mcu_w, si;
        for (si = 0; si < nscan; si++) {
            Comp *c = &p->comp[scan_comp[si]];
            int v, hh;
            for (v = 0; v < c->vi; v++)
                for (hh = 0; hh < c->hi; hh++) {
                    int bx = mcx * c->hi + hh, by = mcy * c->vi + v;
                    if (!decode_block(p, c, bx, by, al)) return 0;
                }
        }
        if (p->restart_interval &&
            ((mcu + 1) % p->restart_interval) == 0 &&
            (mcu + 1) < total_mcu) {
            int r = sync_restart(p);
            if (r == 0) return 0;
            if (r == 2) return 1;          /* scan ended early but ok */
            for (i = 0; i < p->ncomp; i++) p->comp[i].pred = 0;
            bits_reset(p);
        }
    }
    return 1;
}

/* ---- YCbCr -> RGB (integer, BT.601 full range) ---- */
static void ycc_to_rgb(int y, int cb, int cr, uint8_t *rgb) {
    int r, g, b;
    cb -= 128; cr -= 128;
    r = y + ((91881 * cr) >> 16);
    g = y - ((22554 * cb + 46802 * cr) >> 16);
    b = y + ((116130 * cb) >> 16);
    rgb[0] = (uint8_t)clamp8(r);
    rgb[1] = (uint8_t)clamp8(g);
    rgb[2] = (uint8_t)clamp8(b);
}

/* Compose output RGB on the luma block grid from the decoded planes. */
static int compose_output(Pj *p, int *out_w, int *out_h) {
    int ow = p->mcu_w * p->hmax, oh = p->mcu_h * p->vmax, x, y;
    size_t need = (size_t)ow * oh * 3;
    if (need > p->out_cap) return 0;
    for (y = 0; y < oh; y++)
        for (x = 0; x < ow; x++) {
            uint8_t *px = p->out + ((size_t)y * ow + x) * 3;
            if (p->ncomp == 1) {
                Comp *c = &p->comp[0];
                int sx = x * c->hi / p->hmax, sy = y * c->vi / p->vmax;
                if (sx >= c->bw) sx = c->bw - 1;
                if (sy >= c->bh) sy = c->bh - 1;
                px[0] = px[1] = px[2] = c->plane[sy * c->bw + sx];
            } else {
                int yv = 128, cbv = 128, crv = 128, k;
                for (k = 0; k < p->ncomp && k < 3; k++) {
                    Comp *c = &p->comp[k];
                    int sx = x * c->hi / p->hmax, sy = y * c->vi / p->vmax, v;
                    if (sx >= c->bw) sx = c->bw - 1;
                    if (sy >= c->bh) sy = c->bh - 1;
                    v = c->plane[sy * c->bw + sx];
                    if (k == 0) yv = v; else if (k == 1) cbv = v; else crv = v;
                }
                ycc_to_rgb(yv, cbv, crv, px);
            }
        }
    *out_w = ow;
    *out_h = oh;
    return 1;
}

/* Handle one SOS. Returns: 1 = DC scan decoded (done), 2 = non-DC scan
 * skipped (keep parsing), 0 = error. */
static int handle_sos(Pj *p, int *out_w, int *out_h) {
    int ns = rd(p), scan_comp[MAX_COMPS], ss, se, ah_al, ah, al, j;
    if (ns < 1 || ns > p->ncomp) return 0;
    for (j = 0; j < ns; j++) {
        int cs = rd(p), tdta = rd(p), ci;
        if (cs < 0 || tdta < 0) return 0;
        ci = comp_by_id(p, cs);
        if (ci < 0) return 0;
        p->comp[ci].dc_tbl = tdta >> 4;
        if (p->comp[ci].dc_tbl > 3) return 0;
        scan_comp[j] = ci;
    }
    ss = rd(p); se = rd(p); ah_al = rd(p);
    if (ss < 0 || se < 0 || ah_al < 0) return 0;
    ah = ah_al >> 4; al = ah_al & 0xF;

    if (!(ss == 0 && se == 0 && ah == 0)) {
        /* Not the first DC scan: skip its entropy data up to next marker. */
        int prev = 0, c;
        for (;;) {
            c = rd(p);
            if (c < 0) return 0;
            if (prev == 0xFF && c != 0x00 && c != 0xFF &&
                !(c >= 0xD0 && c <= 0xD7)) {
                unread(p, c); unread(p, 0xFF);
                return 2;
            }
            prev = c;
        }
    }

    /* First DC scan: set up planes, decode, compose. */
    for (j = 0; j < p->ncomp; j++) {
        Comp *c = &p->comp[j];
        c->bw = p->mcu_w * c->hi;
        c->bh = p->mcu_h * c->vi;
        if (c->bw > PJ_MAX_BLOCKS_W || c->bh > PJ_MAX_BLOCKS_H) return 0;
        c->plane = p->planes + (size_t)j * PJ_MAX_BLOCKS_W * PJ_MAX_BLOCKS_H;
    }
    for (j = 0; j < ns; j++) {
        Comp *c = &p->comp[scan_comp[j]];
        if (!p->dc_huff[c->dc_tbl].defined) return 0;
        if (!p->qt_defined[c->qt]) return 0;
    }
    if (!decode_dc_scan(p, scan_comp, ns, al)) return 0;
    if (!compose_output(p, out_w, out_h)) return 0;
    return 1;
}

/* ---- top-level decode ---- */

int pj_decode_dc(PjSource *src, uint8_t *out, size_t out_cap,
                 int *out_w, int *out_h) {
    static uint8_t planes[MAX_COMPS * PJ_MAX_BLOCKS_W * PJ_MAX_BLOCKS_H];
    Pj pj;
    Pj *p = &pj;
    int i, b0, b1, len, got_sof2 = 0;

    for (i = 0; i < (int)sizeof(Pj); i++) ((uint8_t *)p)[i] = 0;
    p->src = src;
    p->planes = planes;
    p->out = out;
    p->out_cap = out_cap;

    b0 = rd(p); b1 = rd(p);
    if (b0 != 0xFF || b1 != M_SOI) return 0;

    for (;;) {
        int m;
        b0 = rd(p);
        if (b0 < 0) return 0;
        if (b0 != 0xFF) continue;           /* resync to next 0xFF */
        do { m = rd(p); } while (m == 0xFF);
        if (m < 0) return 0;

        if (m == M_EOI) return 0;
        /* reject baseline/sequential/arithmetic SOF markers */
        if (m == M_SOF0 || m == M_SOF1 ||
            (m >= 0xC3 && m <= 0xCF && m != M_DHT && m != M_SOF2))
            return 0;

        if (!rd_u16(p, &len) || len < 2) return 0;

        if (m == M_DQT) {
            if (!parse_dqt(p, len)) return 0;
        } else if (m == M_DHT) {
            if (!parse_dht(p, len)) return 0;
        } else if (m == M_DRI) {
            if (!rd_u16(p, &p->restart_interval)) return 0;
        } else if (m == M_SOF2) {
            if (!parse_sof2(p)) return 0;
            got_sof2 = 1;
        } else if (m == M_SOS) {
            int r;
            if (!got_sof2) return 0;
            r = handle_sos(p, out_w, out_h);
            if (r == 1) return 1;
            if (r == 0) return 0;
            /* r == 2: non-DC scan skipped, continue marker loop */
        } else {
            int n = len - 2;                /* APPn / COM / other: skip */
            while (n-- > 0) if (rd(p) < 0) return 0;
        }
    }
}
