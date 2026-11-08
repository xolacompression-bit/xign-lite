/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
/* ljpeg.c - Xign Lite: a baseline JPEG read down to its DCT coefficients and written back byte for byte (step J1,
 * 2026-11-09). Written from ISO/IEC 10918-1 / ITU-T T.81 (1992): markers (B.1-B.2), Huffman tables (C), the sequential
 * Huffman coding of DC differences and AC run/size pairs (F.1.2), byte stuffing and restart markers (F.1.2.3, B.2.4.4).
 * Lite's own code (Apache 2.0). No copy check, no block classes, nothing from other recompressors. */
#include "ljpeg.h"
#include <stdlib.h>
#include <string.h>

const char *ljpeg_reason(int c) {
    static const char *r[] = {"handled", "not a JPEG", "progressive", "arithmetic-coded", "not 8-bit", "several scans",
                              "damaged", "written back differently", "out of memory", "other layout"};
    return c >= 0 && c <= LJ_OTHER ? r[c] : "?";
}

typedef struct { uint8_t bits[17]; uint8_t val[256]; int nval;
                 int32_t maxcode[18]; int32_t valptr[17]; int32_t mincode[17];   /* decoding (F.2.2.3) */
                 uint16_t code[256]; uint8_t size[256]; int have;                  /* encoding (C.2) */
                 uint16_t look[256]; } huff_t;     /* decoding by the next 8 bits: (length << 8 | value), 0 = longer code */

static void huff_build(huff_t *h) {
    int k = 0, code = 0;
    memset(h->size, 0, sizeof h->size);
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k; h->mincode[l] = code;
        for (int i = 0; i < h->bits[l]; i++, k++) { h->code[h->val[k]] = (uint16_t)code; h->size[h->val[k]] = (uint8_t)l; code++; }
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    memset(h->look, 0, sizeof h->look);
    k = 0;
    for (int l = 1; l <= 8; l++) for (int i = 0; i < h->bits[l]; i++, k++) {
        int c = h->code[h->val[k]] << (8 - l);
        for (int j = 0; j < (1 << (8 - l)); j++) h->look[c + j] = (uint16_t)(l << 8 | h->val[k]);
    }
    h->have = 1;
}

typedef struct {
    huff_t dc[4], ac[4];
    int ncomp, X, Y, restart, scan_n, scomp[4], td[4], ta[4];
    int H[4], V[4], cid[4], hmax, vmax;
    size_t sos_end;      /* first byte of the scan data */
} hdr_t;

static int u16(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* the header: every marker segment up to and including the one SOS; tables must come before it */
static int parse_header(const uint8_t *p, size_t n, hdr_t *J) {
    memset(J, 0, sizeof *J);
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return LJ_NOT_JPEG;
    size_t i = 2; int sof = 0;
    for (;;) {
        if (i + 4 > n) return LJ_DAMAGED;
        if (p[i] != 0xFF) return LJ_DAMAGED;
        uint8_t m = p[i + 1];
        if (m == 0xFF) { i++; continue; }                         /* fill bytes */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) return LJ_OTHER;
        if (m == 0xD9) return LJ_DAMAGED;
        size_t L = (size_t)u16(p + i + 2);
        if (L < 2 || i + 2 + L > n) return LJ_DAMAGED;
        const uint8_t *s = p + i + 4; size_t sl = L - 2;
        if (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE) return LJ_PROGRESSIVE;
        if ((m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF) || m == 0xCC) return LJ_ARITHMETIC;
        if (m == 0xC3 || m == 0xC5 || m == 0xC7) return LJ_OTHER;   /* lossless / hierarchical */
        if (m == 0xC0 || m == 0xC1) {
            if (sof) return LJ_OTHER;
            if (sl < 6) return LJ_DAMAGED;
            if (s[0] != 8) return LJ_NOT_8BIT;
            J->Y = u16(s + 1); J->X = u16(s + 3); J->ncomp = s[5];
            if (J->X == 0 || J->Y == 0 || J->ncomp < 1 || J->ncomp > 4 || sl != 6 + 3u * J->ncomp) return LJ_OTHER;
            for (int c = 0; c < J->ncomp; c++) {
                J->cid[c] = s[6 + 3 * c]; J->H[c] = s[7 + 3 * c] >> 4; J->V[c] = s[7 + 3 * c] & 15;
                if (J->H[c] < 1 || J->H[c] > 4 || J->V[c] < 1 || J->V[c] > 4) return LJ_OTHER;
                if (J->H[c] > J->hmax) J->hmax = J->H[c];
                if (J->V[c] > J->vmax) J->vmax = J->V[c];
            }
            sof = 1;
        } else if (m == 0xC4) {
            size_t q = 0;
            while (q < sl) {
                if (q + 17 > sl) return LJ_DAMAGED;
                int tc = s[q] >> 4, th = s[q] & 15;
                if (tc > 1 || th > 3) return LJ_OTHER;
                huff_t *h = tc ? &J->ac[th] : &J->dc[th];
                int tot = 0; h->bits[0] = 0;
                for (int l = 1; l <= 16; l++) { h->bits[l] = s[q + l]; tot += s[q + l]; }
                if (tot > 256 || q + 17 + tot > sl) return LJ_DAMAGED;
                memcpy(h->val, s + q + 17, tot); h->nval = tot;
                huff_build(h);
                q += 17 + tot;
            }
        } else if (m == 0xDD) {
            if (sl != 2) return LJ_DAMAGED;
            J->restart = u16(s);
        } else if (m == 0xDA) {
            if (!sof || sl < 1) return LJ_OTHER;
            J->scan_n = s[0];
            if (J->scan_n < 1 || J->scan_n > 4 || sl != 4 + 2u * J->scan_n) return LJ_DAMAGED;
            if (!(J->scan_n == J->ncomp || (J->ncomp == 1))) return LJ_SCANS;
            for (int k = 0; k < J->scan_n; k++) {
                int c = -1; for (int q = 0; q < J->ncomp; q++) if (J->cid[q] == s[1 + 2 * k]) c = q;
                if (c < 0) return LJ_OTHER;
                J->scomp[k] = c; J->td[k] = s[2 + 2 * k] >> 4; J->ta[k] = s[2 + 2 * k] & 15;
                if (J->td[k] > 3 || J->ta[k] > 3 || !J->dc[J->td[k]].have || !J->ac[J->ta[k]].have) return LJ_OTHER;
            }
            const uint8_t *e = s + 1 + 2 * J->scan_n;
            if (e[0] != 0 || e[1] != 63 || e[2] != 0) return LJ_OTHER;   /* Ss=0, Se=63, Ah=Al=0 */
            J->sos_end = i + 2 + L;
            return LJ_OK;
        }
        i += 2 + L;
    }
}

/* ---------------------------------------------------------------- the block layout of the one scan */
typedef struct { int nmcu, per_mcu, mcux; int bc[10], bx[10], by[10]; int ncb; } layout_t;

static int layout(const hdr_t *J, layout_t *Lo) {
    memset(Lo, 0, sizeof *Lo);
    if (J->scan_n == 1) {   /* one component: one block per MCU (A.2.2) */
        int c = J->scomp[0];
        int cw = (J->X * J->H[c] + J->hmax - 1) / J->hmax, ch = (J->Y * J->V[c] + J->vmax - 1) / J->vmax;
        int bw = (cw + 7) / 8, bh = (ch + 7) / 8;
        Lo->nmcu = bw * bh; Lo->per_mcu = 1; Lo->mcux = bw; Lo->ncb = 1; Lo->bc[0] = 0;
        return 0;
    }
    int mx = (J->X + 8 * J->hmax - 1) / (8 * J->hmax), my = (J->Y + 8 * J->vmax - 1) / (8 * J->vmax);
    Lo->nmcu = mx * my; Lo->mcux = mx; int k = 0;
    for (int q = 0; q < J->scan_n; q++) { int c = J->scomp[q];
        for (int y = 0; y < J->V[c]; y++) for (int x = 0; x < J->H[c]; x++) { if (k >= 10) return 1; Lo->bc[k] = q; Lo->bx[k] = x; Lo->by[k] = y; k++; } }
    Lo->per_mcu = k; Lo->ncb = k;
    return 0;
}

/* ---------------------------------------------------------------- reading the scan bits */
typedef struct { const uint8_t *p; size_t pos, end; uint32_t acc; int nb; int marker; int padbit; int padseen, padmix; } br_t;

static int fill(br_t *b) {             /* one more byte into the bit buffer; stuffed 0xFF00 -> 0xFF; a marker stops */
    if (b->marker) return 0;
    if (b->pos >= b->end) return 0;
    uint8_t c = b->p[b->pos];
    if (c == 0xFF) {
        if (b->pos + 1 >= b->end) return 0;
        if (b->p[b->pos + 1] == 0x00) { b->pos += 2; } else { b->marker = 1; return 0; }
    } else b->pos++;
    b->acc = b->acc << 8 | c; b->nb += 8;
    return 1;
}
static int getbit(br_t *b) {
    if (!b->nb && !fill(b)) return -1;
    b->nb--; return (b->acc >> b->nb) & 1;
}
static int getbits(br_t *b, int n, int *v) {
    while (b->nb < n) if (!fill(b)) return -1;
    b->nb -= n; *v = (int)((b->acc >> b->nb) & ((1u << n) - 1)); return 0;
}
static int decode(br_t *b, const huff_t *h) {     /* F.2.2.3, with an 8-bit look-ahead table for the short codes */
    while (b->nb < 8 && fill(b)) {}
    if (b->nb >= 8) {
        uint16_t e = h->look[(b->acc >> (b->nb - 8)) & 0xFF];
        if (e) { b->nb -= e >> 8; return e & 0xFF; }
    }
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        int t = getbit(b); if (t < 0) return -1;
        code = code << 1 | t;
        if (h->bits[l] && code <= h->maxcode[l] && code >= h->mincode[l]) return h->val[h->valptr[l] + code - h->mincode[l]];
    }
    return -2;
}
static int extend(int v, int s) { return s && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }   /* F.2.2.1 */

/* the bits left in the current byte at an interval's end are padding: all must be the same */
static void eat_padding(br_t *b) {
    while (b->nb & 7) { int t = (b->acc >> (b->nb - 1)) & 1; b->nb--;
        if (!b->padseen) { b->padbit = t; b->padseen = 1; } else if (t != b->padbit) b->padmix = 1; }
    if (b->nb) b->padmix = 1;   /* whole bytes left over: not padding */
}

/* ---------------------------------------------------------------- writing the scan bits */
typedef struct { uint8_t *o; size_t n, cap; uint32_t acc; int nb; int err; } bw_t;
static void putbyte(bw_t *w, uint8_t c) {
    if (w->n + 2 > w->cap) { size_t k = w->cap * 2 + 4096; uint8_t *q = realloc(w->o, k); if (!q) { w->err = 1; return; } w->o = q; w->cap = k; }
    w->o[w->n++] = c; if (c == 0xFF) w->o[w->n++] = 0x00;
}
static void putbits(bw_t *w, uint32_t v, int n) {          /* n <= 16 */
    w->acc = w->acc << n | (v & ((1u << n) - 1)); w->nb += n;
    while (w->nb >= 8) { w->nb -= 8; putbyte(w, (uint8_t)(w->acc >> w->nb)); }
    w->acc &= (1u << w->nb) - 1;
}
static void flushpad(bw_t *w, int padbit) { while (w->nb) putbits(w, (uint32_t)padbit, 1); }
static void putmarker(bw_t *w, uint8_t m) {
    if (w->n + 2 > w->cap) { size_t k = w->cap * 2 + 4096; uint8_t *q = realloc(w->o, k); if (!q) { w->err = 1; return; } w->o = q; w->cap = k; }
    w->o[w->n++] = 0xFF; w->o[w->n++] = m;
}
static int bitsize(int v) { if (v < 0) v = -v; int s = 0; while (v) { s++; v >>= 1; } return s; }

static int write_scan(const hdr_t *J, const layout_t *Lo, const int16_t *coef, int padbit, uint8_t **out, size_t *on) {
    bw_t w = {0}; w.cap = 1 << 16; w.o = malloc(w.cap); if (!w.o) return LJ_MEM;
    int pred[4] = {0}; size_t blk = 0; int rst = 0;
    for (int m = 0; m < Lo->nmcu && !w.err; m++) {
        if (J->restart && m && m % J->restart == 0) { flushpad(&w, padbit); putmarker(&w, (uint8_t)(0xD0 + (rst & 7))); rst++; memset(pred, 0, sizeof pred); }
        for (int k = 0; k < Lo->per_mcu; k++, blk++) {
            int q = Lo->bc[k]; const huff_t *dc = &J->dc[J->td[q]], *ac = &J->ac[J->ta[q]];
            const int16_t *z = coef + blk * 64;
            int d = z[0] - pred[q]; pred[q] = z[0];
            int s = bitsize(d); if (s > 11 || !dc->size[s]) { w.err = 2; break; }
            putbits(&w, dc->code[s], dc->size[s]); if (s) putbits(&w, (uint32_t)(d < 0 ? d - 1 : d) & ((1u << s) - 1), s);
            int run = 0;
            for (int i = 1; i < 64; i++) {
                int v = z[i];
                if (!v) { run++; continue; }
                while (run > 15) { if (!ac->size[0xF0]) { w.err = 2; break; } putbits(&w, ac->code[0xF0], ac->size[0xF0]); run -= 16; }
                int t = bitsize(v); int sym = run << 4 | t; if (t > 10 || !ac->size[sym]) { w.err = 2; break; }
                putbits(&w, ac->code[sym], ac->size[sym]); putbits(&w, (uint32_t)(v < 0 ? v - 1 : v) & ((1u << t) - 1), t);
                run = 0;
            }
            if (run) { if (!ac->size[0x00]) { w.err = 2; break; } putbits(&w, ac->code[0x00], ac->size[0x00]); }
        }
    }
    flushpad(&w, padbit);
    if (w.err) { free(w.o); return w.err == 1 ? LJ_MEM : LJ_NOT_SAME; }
    *out = w.o; *on = w.n; return LJ_OK;
}

int ljpeg_write_scan(const uint8_t *p, const ljpeg_info *I, const int16_t *coef, uint8_t **out, size_t *on) {
    hdr_t J; int r = parse_header(p, I->head_len, &J);
    if (r) return r;
    layout_t Lo; if (layout(&J, &Lo)) return LJ_OTHER;
    /* the header must give exactly the blocks the coefficients were coded for (a damaged header never reads past them) */
    if ((int64_t)Lo.nmcu * Lo.per_mcu != I->nblocks || Lo.per_mcu != I->per_mcu || Lo.mcux != I->mcux) return LJ_DAMAGED;
    for (int k = 0; k < Lo.per_mcu; k++)
        if (Lo.bc[k] != I->blk_comp[k] || Lo.bx[k] != I->blk_x[k] || Lo.by[k] != I->blk_y[k]) return LJ_DAMAGED;
    return write_scan(&J, &Lo, coef, I->pad_bit, out, on);
}

int ljpeg_read(const uint8_t *p, size_t n, ljpeg_info *I, int16_t **coef) {
    memset(I, 0, sizeof *I); *coef = NULL;
    hdr_t J; int r = parse_header(p, n, &J);
    if (r) return r;
    layout_t Lo; if (layout(&J, &Lo)) return LJ_OTHER;
    size_t nblk = (size_t)Lo.nmcu * Lo.per_mcu;
    if (nblk > ((size_t)1 << 26)) return LJ_OTHER;
    int16_t *cf = calloc(nblk * 64, sizeof *cf);
    if (!cf) return LJ_MEM;
    br_t b = {0}; b.p = p; b.pos = J.sos_end; b.end = n;
    int pred[4] = {0}; size_t blk = 0; int rst = 0, bad = 0;
    for (int m = 0; m < Lo.nmcu && !bad; m++) {
        if (J.restart && m && m % J.restart == 0) {           /* the interval ends: padding, then RSTn */
            eat_padding(&b);
            if (b.pos + 1 >= n || b.p[b.pos] != 0xFF || b.p[b.pos + 1] != 0xD0 + (rst & 7)) { bad = LJ_NOT_SAME; break; }
            b.pos += 2; b.marker = 0; b.nb = 0; b.acc = 0; rst++; memset(pred, 0, sizeof pred);
        }
        for (int k = 0; k < Lo.per_mcu && !bad; k++, blk++) {
            int q = Lo.bc[k]; const huff_t *dc = &J.dc[J.td[q]], *ac = &J.ac[J.ta[q]];
            int16_t *z = cf + blk * 64;
            int s = decode(&b, dc); if (s < 0 || s > 11) { bad = LJ_DAMAGED; break; }
            int v = 0; if (s && getbits(&b, s, &v)) { bad = LJ_DAMAGED; break; }
            int dcv = pred[q] + extend(v, s); if (dcv < -32768 || dcv > 32767) { bad = LJ_DAMAGED; break; }
            z[0] = (int16_t)dcv; pred[q] = dcv;
            for (int i = 1; i < 64; ) {
                int rs = decode(&b, ac); if (rs < 0) { bad = LJ_DAMAGED; break; }
                int rr = rs >> 4, ss = rs & 15;
                if (!ss) { if (rr == 15) { i += 16; if (i > 64) { bad = LJ_DAMAGED; break; } continue; } break; }   /* ZRL or EOB */
                i += rr; if (i > 63) { bad = LJ_DAMAGED; break; }
                if (getbits(&b, ss, &v)) { bad = LJ_DAMAGED; break; }
                z[i++] = (int16_t)extend(v, ss);
            }
        }
    }
    if (!bad) {
        eat_padding(&b);                                       /* the scan must end right where the next marker begins */
        if (b.pos + 1 >= n || b.p[b.pos] != 0xFF || b.p[b.pos + 1] == 0x00 || (b.p[b.pos + 1] >= 0xD0 && b.p[b.pos + 1] <= 0xD7)) bad = LJ_NOT_SAME;
        if (b.padmix) bad = LJ_NOT_SAME;
    }
    if (bad) { free(cf); return bad; }
    size_t scan_end = b.pos;                                   /* the 0xFF of the marker after the scan */
    I->width = J.X; I->height = J.Y; I->ncomp = J.ncomp; I->nblocks = (int)nblk; I->mcus = Lo.nmcu; I->restart = J.restart;
    for (int c = 0; c < J.ncomp; c++) { I->hs[c] = J.H[c]; I->vs[c] = J.V[c]; }
    I->head_len = J.sos_end; I->scan_off = J.sos_end; I->scan_len = scan_end - J.sos_end; I->tail_len = n - scan_end;
    I->pad_bit = b.padseen ? b.padbit : 1;
    I->per_mcu = Lo.per_mcu; I->mcux = Lo.mcux;
    for (int k = 0; k < Lo.per_mcu; k++) { I->blk_comp[k] = Lo.bc[k]; I->blk_x[k] = Lo.bx[k]; I->blk_y[k] = Lo.by[k]; }
    /* the check: written back, the scan must be the same bytes */
    uint8_t *w = NULL; size_t wn = 0;
    r = write_scan(&J, &Lo, cf, I->pad_bit, &w, &wn);
    if (r) { free(cf); return r; }
    int same = wn == I->scan_len && !memcmp(w, p + J.sos_end, wn);
    free(w);
    if (!same) { free(cf); return LJ_NOT_SAME; }
    *coef = cf;
    return LJ_OK;
}
