/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
/* lcoef.c - Xign Lite: the JPEG coefficient coder (see lcoef.h). Lite's own code (Apache 2.0). */
#include "lcoef.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- the binary range coder (11-bit odds, step 1/32) */
#define PB 11
#define PONE (1u << PB)
#define MOVE 5
typedef uint16_t prob;
typedef struct { uint64_t low; uint32_t range; uint8_t cache; uint64_t csize; uint8_t *o; size_t n, cap; int err; } renc;
typedef struct { const uint8_t *p, *e; uint32_t range, code; int err; } rdec;

static void out1(renc *e, uint8_t c) {
    if (e->n == e->cap) { size_t k = e->cap * 2 + 65536; uint8_t *q = realloc(e->o, k); if (!q) { e->err = 1; return; } e->o = q; e->cap = k; }
    e->o[e->n++] = c;
}
static void shift_low(renc *e) {
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32)) {
        uint8_t carry = (uint8_t)(e->low >> 32), t = e->cache;
        do { out1(e, (uint8_t)(t + carry)); t = 0xFF; } while (--e->csize);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->csize++; e->low = (e->low & 0x00FFFFFFu) << 8;
}
static void ebit(renc *e, prob *p, int bit) {
    uint32_t b = (e->range >> PB) * *p;
    if (!bit) { e->range = b; *p += (PONE - *p) >> MOVE; }
    else { e->low += b; e->range -= b; *p -= *p >> MOVE; }
    while (e->range < (1u << 24)) { e->range <<= 8; shift_low(e); }
}
static void eraw(renc *e, int bit) {       /* a fixed half-and-half decision (the sign, as in the standard) */
    e->range >>= 1; if (bit) e->low += e->range;
    while (e->range < (1u << 24)) { e->range <<= 8; shift_low(e); }
}
static void eflush(renc *e) { for (int i = 0; i < 5; i++) shift_low(e); }

static uint8_t in1(rdec *d) { if (d->p < d->e) return *d->p++; d->err = 1; return 0; }
static void dinit(rdec *d, const uint8_t *p, size_t n) { d->p = p; d->e = p + n; d->range = 0xFFFFFFFFu; d->code = 0; d->err = 0;
    for (int i = 0; i < 5; i++) d->code = d->code << 8 | in1(d); }
static int dbit(rdec *d, prob *p) {
    uint32_t b = (d->range >> PB) * *p; int bit;
    if (d->code < b) { d->range = b; *p += (PONE - *p) >> MOVE; bit = 0; }
    else { d->code -= b; d->range -= b; *p -= *p >> MOVE; bit = 1; }
    while (d->range < (1u << 24)) { d->range <<= 8; d->code = d->code << 8 | in1(d); }
    return bit;
}
static int draw(rdec *d) {
    d->range >>= 1; int bit = 0; if (d->code >= d->range) { d->code -= d->range; bit = 1; }
    while (d->range < (1u << 24)) { d->range <<= 8; d->code = d->code << 8 | in1(d); }
    return bit;
}

/* one coder for both ways: packing passes the bit and gets it back, unpacking gets the bit - the same steps either way */
typedef struct { renc e; rdec d; int dec; } cod;
static int cbit(cod *c, prob *p, int bit) { if (c->dec) return dbit(&c->d, p); ebit(&c->e, p, bit); return bit; }
static int craw(cod *c, int bit) { if (c->dec) return draw(&c->d); eraw(&c->e, bit); return bit; }
static int cerr(const cod *c) { return c->dec ? c->d.err : c->e.err; }

/* ---------------------------------------------------------------- the model */
typedef struct { prob x[16], m[16]; } magbins;                 /* size class (unary) and extra bits, per class */
#define NQS 7      /* the outside clue: |above| + |left| at the same place, in 7 steps */
#define NQI 3      /* the inside clue: the previous place of the same block, 0 / 1 / 2+ */
#define NQR 9      /* how many non-zero values are still to come */
#define NQN 12     /* the neighbours' non-zero counts */
#define NKG 6      /* places grouped for the sizes */
typedef struct {
    prob dz[5], ds[5]; magbins dm[5];                          /* DC: zero, sign, size - chosen by the last DC difference */
    prob nz0[NQN]; magbins nzm[NQN];                           /* the block's non-zero AC count: zero?, then size class and bits */
    prob zf[64][NQR][NQS][NQI];                                /* non-zero at place k */
    magbins am[NKG][NQS][NQI];                                 /* AC size */
} model;

static void minit(model *m) { prob *p = (prob *)m; for (size_t i = 0; i < sizeof *m / sizeof *p; i++) p[i] = PONE / 2; }
static int dc_ctx(int d) { return d == 0 ? 0 : d == 1 ? 1 : d == -1 ? 2 : d > 1 ? 3 : 4; }   /* the standard's L = 0, U = 1 */
static int blen(unsigned v) { int b = 0; while (v) { b++; v >>= 1; } return b; }
static int qs(int v) { return v < 3 ? v : v < 5 ? 3 : v < 9 ? 4 : v < 17 ? 5 : 6; }
static int qi(int v) { return v < 2 ? v : 2; }
static int qr(int v) { return v < 5 ? v - 1 : v < 7 ? 4 : v < 10 ? 5 : v < 15 ? 6 : v < 23 ? 7 : 8; }
static int qn(int v) { return v < 5 ? v : v < 7 ? 5 : v < 10 ? 6 : v < 14 ? 7 : v < 20 ? 8 : v < 28 ? 9 : v < 40 ? 10 : 11; }
static int kg(int k) { return k < 3 ? 0 : k < 6 ? 1 : k < 10 ? 2 : k < 15 ? 3 : k < 28 ? 4 : 5; }
/* zig-zag place k -> the first column of the block (a vertical frequency): k = 2, 3, 9, 10, 20, 21, 35 (and 0, the DC) */
static const uint8_t COL0[64] = {1,0,1,1,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1};

static unsigned cmag(cod *c, magbins *b, unsigned sz) {       /* sz = |value| - 1 */
    if (!cbit(c, &b->x[0], sz != 0)) return 0;
    int n = c->dec ? 0 : blen(sz), k = 1;                      /* size class 1 .. 15, unary */
    while (k < 15 && cbit(c, &b->x[k], k < n)) k++;
    unsigned v = 1;
    for (int i = k - 2; i >= 0; i--) v = v << 1 | (unsigned)cbit(c, &b->m[k], (sz >> i) & 1);
    return v;
}

/* the block grid of each colour part: a block's neighbours above and to the left (-1 = none) */
typedef struct { int pm, mcux, comp[10], bx[10], by[10], H[4], V[4], jmap[4][16][16]; } grid;
static int grid_make(grid *G, const lcoef_layout *L) {
    memset(G, 0, sizeof *G);
    if (L->per_mcu < 1 || L->per_mcu > 10 || L->mcux < 1) return 1;
    G->pm = L->per_mcu; G->mcux = L->mcux;
    memset(G->jmap, 0xFF, sizeof G->jmap);
    for (int j = 0; j < G->pm; j++) {
        int c = L->comp[j], x = L->bx[j], y = L->by[j];
        if (c < 0 || c > 3 || x < 0 || x > 15 || y < 0 || y > 15 || G->jmap[c][y][x] >= 0) return 1;
        G->comp[j] = c; G->bx[j] = x; G->by[j] = y; G->jmap[c][y][x] = j;
        if (x + 1 > G->H[c]) G->H[c] = x + 1;
        if (y + 1 > G->V[c]) G->V[c] = y + 1;
    }
    for (int j = 0; j < G->pm; j++) {                          /* every part's blocks fill its H x V box, row by row */
        int c = G->comp[j];
        for (int y = 0; y < G->V[c]; y++) for (int x = 0; x < G->H[c]; x++) if (G->jmap[c][y][x] < 0) return 1;
    }
    return 0;
}
static void neighbours(const grid *G, int b, int *up, int *left) {
    int mcu = b / G->pm, j = b % G->pm, c = G->comp[j], H = G->H[c], V = G->V[c];
    int mx = mcu % G->mcux, my = mcu / G->mcux, gx = mx * H + G->bx[j], gy = my * V + G->by[j];
    *up = *left = -1;
    if (gy > 0) { int y = gy - 1; *up = ((y / V) * G->mcux + mx) * G->pm + G->jmap[c][y % V][G->bx[j]]; }
    if (gx > 0) { int x = gx - 1; *left = (my * G->mcux + x / H) * G->pm + G->jmap[c][G->by[j]][x % H]; }
    if (*up >= b) *up = -1;
    if (*left >= b) *left = -1;
}

static int run(cod *C, int16_t *coef, int nblocks, const lcoef_layout *L) {
    grid G; if (grid_make(&G, L)) return 1;
    model *M = calloc(4, sizeof *M); uint8_t *nzs = malloc(nblocks > 0 ? (size_t)nblocks : 1);
    if (!M || !nzs) { free(M); free(nzs); return -1; }
    for (int q = 0; q < 4; q++) minit(&M[q]);
    static const int16_t ZERO[64];
    int pred[4] = {0}, last[4] = {0}, bad = 0;
    for (int b = 0; b < nblocks && !cerr(C) && !bad; b++) {
        int q = G.comp[b % G.pm]; model *m = &M[q]; int16_t *z = coef + (size_t)b * 64;
        int up, lf; neighbours(&G, b, &up, &lf);
        const int16_t *A = up >= 0 ? coef + (size_t)up * 64 : ZERO, *Lb = lf >= 0 ? coef + (size_t)lf * 64 : ZERO;
        /* DC first (the standard's model) */
        int c = dc_ctx(last[q]), d = C->dec ? 0 : z[0] - pred[q];
        if (cbit(C, &m->dz[c], d != 0)) {
            int neg = cbit(C, &m->ds[c], d < 0);
            unsigned sz = cmag(C, &m->dm[c], (unsigned)(d < 0 ? -d : d) - 1);
            d = neg ? -(int)(sz + 1) : (int)(sz + 1);
        } else d = 0;
        int v0 = pred[q] + d;
        if (v0 < -32768 || v0 > 32767) { bad = 1; break; }
        if (C->dec) { memset(z, 0, 64 * sizeof *z); z[0] = (int16_t)v0; }
        last[q] = d; pred[q] = v0;
        /* how many AC values are not zero (the neighbours' counts as the clue; a missing neighbour counts 0) */
        int nz = 0;
        if (!C->dec) for (int k = 1; k < 64; k++) nz += z[k] != 0;
        int nn = ((up >= 0 ? nzs[up] : 0) + (lf >= 0 ? nzs[lf] : 0) + 1) / 2;
        /* coded the standard's way (T.81 F.1.4.3): zero or not, then the size class step by step, then the bits below it */
        int qc = qn(nn);
        if (cbit(C, &m->nz0[qc], nz != 0)) {
            unsigned v = cmag(C, &m->nzm[qc], (unsigned)(nz > 0 ? nz - 1 : 0)) + 1;
            if (v > 63) { bad = 1; break; }
            nz = (int)v;
        } else nz = 0;
        nzs[b] = (uint8_t)nz;
        /* the AC values in zig-zag order until all non-zero ones are coded */
        int rem = nz;
        for (int k = 1; k < 64 && rem > 0; k++) {
            int s = COL0[k] ? 0 : qs(abs(A[k]) + abs(Lb[k])), in = qi(abs(z[k - 1]));
            if (k == 1) in = 0;
            int v = C->dec ? 0 : z[k], on;
            if (64 - k == rem) on = 1;                          /* every place left is non-zero */
            else on = cbit(C, &m->zf[k][qr(rem)][s][in], v != 0);
            if (!on) continue;
            int neg = craw(C, v < 0);                           /* the sign: half and half, as in the standard */
            unsigned sz = cmag(C, &m->am[kg(k)][s][in], (unsigned)(v < 0 ? -v : v) - 1);
            if (sz > 32767) { bad = 1; break; }
            if (C->dec) z[k] = (int16_t)(neg ? -(int)(sz + 1) : (int)(sz + 1));
            rem--;
        }
    }
    free(M); free(nzs);
    return bad || cerr(C) ? 1 : 0;
}

int lcoef_encode(const int16_t *coef, int nblocks, const lcoef_layout *L, uint8_t **out, size_t *on) {
    cod C; memset(&C, 0, sizeof C); C.e.range = 0xFFFFFFFFu; C.e.csize = 1;
    int r = run(&C, (int16_t *)coef, nblocks, L);               /* packing only reads the coefficients */
    if (!r) eflush(&C.e);
    if (r || C.e.err) { free(C.e.o); return r < 0 || C.e.err ? -1 : 1; }
    *out = C.e.o; *on = C.e.n; return 0;
}

int lcoef_decode(const uint8_t *in, size_t n, int nblocks, const lcoef_layout *L, int16_t *coef) {
    cod C; memset(&C, 0, sizeof C); C.dec = 1; dinit(&C.d, in, n);
    return run(&C, coef, nblocks, L);
}
