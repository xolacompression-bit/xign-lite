/* The LZ coder (way A) 0.3.2 - a seekable, block-parallel, static-table range coder.
 * Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE).
 * Where every idea comes from: docs/SOURCES.md.
 *
 * The parts and where each comes from (detail: docs/SOURCES.md):
 *   match finder   - the general LZ77 algorithm of RFC 1951 section 4: chained hash table, one-step lazy matching
 *   range coder    - range coding (G. N. N. Martin, 1979), carry-less form (D. Subbotin, 1999)
 *   tables         - semi-static (two-pass) frequency tables: one base set per file, each block's set stored as a
 *                    plain difference from it, every entry written the same way
 *   context groups - order-1 contexts grouped by their statistics (grouping of contexts as in RFC 7932)
 *   lanes          - the tokens of a block go to 1..8 lanes in turn, each lane its own stream (F. Giesen, 2014)
 *   index + check  - byte offsets of whole blocks at the end of the archive; CRC-32C (Castagnoli, 1993) per block
 *   integers only  - the archive is the same on every machine and with any thread count
 */
#include "xola.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef XOLA_STATS
#include <stdio.h>
#endif

/* ------------------------------------------------------------------ constants */
#define TOTB 12
#define TOT (1u << TOTB)
#define NLL 284          /* 256 literals + 16 short lengths + 12 long-length buckets */
#define NDIST 95         /* 92 distance codes (2 bits below the top bit shaped by the table) + 3 repeat codes */
#define REP 92           /* codes 92, 93, 94: the three remembered distances R0, R1, R2 (as in LZX, 1995) */

/* the three remembered distances. Using Rk (k > 0) swaps it with R0; a new distance pushes R0 -> R1 -> R2 (LZX's rule) */
typedef struct { uint32_t r[3]; } reps_t;
static inline int rep_index(const reps_t *R, uint32_t d) { return d == R->r[0] ? 0 : d == R->r[1] ? 1 : d == R->r[2] ? 2 : -1; }
static inline void rep_use(reps_t *R, uint32_t d) {
    int k = rep_index(R, d);
    if (k == 0) return;
    if (k > 0) { R->r[k] = R->r[0]; R->r[0] = d; return; }
    R->r[2] = R->r[1]; R->r[1] = R->r[0]; R->r[0] = d;
}
#define MINLEN 4
#define MAXLEN (MINLEN + 65535)
#define MAXG 16
#define MAXLANES 8
#define HBITS 20
#define FOOT 28
static const uint8_t MAGIC[4] = {'X', 'O', 'L', 'A'};
static const uint8_t MAGIC_END[8] = {'X', 'O', 'L', 'A', 'E', 'N', 'D', '1'};

/* the fixed model for table differences: frequency of each bit-length bucket 0..14 (sums to 4096) */
static const uint16_t BK[15] = {2048, 512, 512, 384, 256, 160, 96, 48, 32, 16, 12, 8, 6, 4, 2};

/* ------------------------------------------------------------------ small helpers */
static inline int bitlen(uint32_t x) { return x ? 32 - __builtin_clz(x) : 0; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

typedef struct { uint8_t *p; size_t n, cap; int err; } buf_t;
static void bput(buf_t *b, const void *d, size_t n) {
    if (b->err) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t *q = realloc(b->p, c);
        if (!q) { b->err = 1; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void bput1(buf_t *b, uint8_t v) { bput(b, &v, 1); }

/* integer log2 in 1/65536 units (deterministic) */
static uint32_t ilog2fx(uint64_t x) {
    if (!x) return 0;
    int e = 63 - __builtin_clzll(x);
    uint64_t m = e >= 31 ? x >> (e - 31) : x << (31 - e);
    uint32_t fr = 0;
    for (int i = 15; i >= 0; i--) { m = (m * m) >> 31; if (m >= (1ull << 32)) { m >>= 1; fr |= 1u << i; } }
    return ((uint32_t)e << 16) | fr;
}

/* ------------------------------------------------------------------ CRC-32C (Castagnoli), plain table form */
static uint32_t crc_tab[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
        crc_tab[i] = c;
    }
}
uint32_t xola_crc32c(uint32_t crc, const uint8_t *p, size_t n) {
    pthread_once(&crc_once, crc_init);
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------ range coder (Martin 1979; carry-less, Subbotin 1999) */
#define RC_TOP (1u << 24)
#define RC_BOT (1u << 16)
typedef struct { uint32_t low, range; buf_t out; } enc_t;
static void enc_init(enc_t *e) { e->low = 0; e->range = 0xFFFFFFFFu; memset(&e->out, 0, sizeof e->out); }
static inline void enc_norm(enc_t *e) {
    while ((e->low ^ (e->low + e->range)) < RC_TOP || (e->range < RC_BOT && ((e->range = (0u - e->low) & (RC_BOT - 1)), 1))) {
        bput1(&e->out, (uint8_t)(e->low >> 24));
        e->low <<= 8; e->range <<= 8;
    }
}
static inline void enc_sym(enc_t *e, uint32_t cum, uint32_t freq) {
    e->range >>= TOTB; e->low += cum * e->range; e->range *= freq; enc_norm(e);
}
static inline void enc_bits(enc_t *e, uint32_t v, int nb) {
    if (nb > 16) { enc_bits(e, v >> 16, nb - 16); v &= 0xFFFF; nb = 16; }
    if (!nb) return;
    e->range >>= nb; e->low += v * e->range; enc_norm(e);
}
static void enc_flush(enc_t *e) { for (int i = 0; i < 4; i++) { bput1(&e->out, (uint8_t)(e->low >> 24)); e->low <<= 8; } }

typedef struct { uint32_t low, range, code; const uint8_t *p, *end; int err; } dec_t;
static inline uint8_t din(dec_t *d) { return d->p < d->end ? *d->p++ : 0; }
static void dec_init(dec_t *d, const uint8_t *p, size_t n) {
    d->p = p; d->end = p + n; d->low = 0; d->range = 0xFFFFFFFFu; d->code = 0; d->err = 0;
    for (int i = 0; i < 4; i++) d->code = (d->code << 8) | din(d);
}
static inline void dec_norm(dec_t *d) {
    while ((d->low ^ (d->low + d->range)) < RC_TOP || (d->range < RC_BOT && ((d->range = (0u - d->low) & (RC_BOT - 1)), 1))) {
        d->code = (d->code << 8) | din(d);
        d->low <<= 8; d->range <<= 8;
    }
}
static inline uint32_t dec_bits(dec_t *d, int nb) {
    if (nb > 16) { uint32_t hi = dec_bits(d, nb - 16); return (hi << 16) | dec_bits(d, 16); }
    if (!nb) return 0;
    uint32_t r = d->range >> nb, v = (d->code - d->low) / r;
    if (v >> nb) { d->err = 1; v = 0; }
    d->low += v * r; d->range = r; dec_norm(d);
    return v;
}

/* ------------------------------------------------------------------ tables */
typedef struct { uint16_t f[NLL]; uint16_t c[NLL + 1]; } tab_t;   /* used for both alphabets (n <= NLL) */
typedef struct { const tab_t *t; uint16_t *lut; } dtab_t;

/* counts -> frequencies that sum to exactly TOT; every counted symbol keeps at least 1.
 * Integer only and fully determined by the counts. With no counts, `fb` is copied. */
static void normalize(const uint32_t *cnt, int n, uint16_t *f, const uint16_t *fb) {
    uint64_t total = 0;
    for (int s = 0; s < n; s++) total += cnt[s];
    if (!total) { memcpy(f, fb, n * sizeof *f); return; }
    int32_t sum = 0;
    for (int s = 0; s < n; s++) {
        uint32_t v = cnt[s] ? (uint32_t)((uint64_t)cnt[s] * TOT / total) : 0;
        if (cnt[s] && !v) v = 1;
        f[s] = (uint16_t)v; sum += v;
    }
    while (sum != (int32_t)TOT) {          /* move one unit at a time on the largest entry (lowest symbol on a tie) */
        int best = -1;
        for (int s = 0; s < n; s++)
            if (f[s] && (sum < (int32_t)TOT || f[s] > 1) && (best < 0 || f[s] > f[best])) best = s;
        if (sum > (int32_t)TOT) { f[best]--; sum--; } else { f[best]++; sum++; }
    }
}
static void cumulate(tab_t *t, int n) {
    t->c[0] = 0;
    for (int s = 0; s < n; s++) t->c[s + 1] = t->c[s] + t->f[s];
}
static void build_lut(const tab_t *t, int n, uint16_t *lut) {
    for (int s = 0; s < n; s++)
        for (uint32_t k = t->c[s]; k < t->c[s + 1]; k++) lut[k] = (uint16_t)s;
}
static uint16_t ONE[NLL], ZERO[NLL];   /* ZERO: reference of the base tables; ONE (all on symbol 0): base for an empty group */
static pthread_once_t flat_once = PTHREAD_ONCE_INIT;
static void flat_init(void) { ONE[0] = (uint16_t)TOT; }

/* table differences: zigzag -> bit-length bucket from the fixed model BK, then the bits below the top bit, plain */
static void enc_delta(enc_t *e, int d) {
    uint32_t z = d >= 0 ? (uint32_t)d << 1 : ((uint32_t)(-d) << 1) - 1;
    int b = bitlen(z), cum = 0;
    for (int i = 0; i < b; i++) cum += BK[i];
    enc_sym(e, cum, BK[b]);
    if (b > 1) enc_bits(e, z & ((1u << (b - 1)) - 1), b - 1);
}
static int dec_delta(dec_t *d) {
    uint32_t r = d->range >> TOTB, f = (d->code - d->low) / r;
    if (f >= TOT) { d->err = 1; f = 0; }
    int b = 0; uint32_t cum = 0;
    while (b < 14 && cum + BK[b] <= f) { cum += BK[b]; b++; }
    d->low += cum * r; d->range = r * BK[b]; dec_norm(d);
    uint32_t z = b ? (1u << (b - 1)) | (b > 1 ? dec_bits(d, b - 1) : 0) : 0;
    return (z & 1) ? -(int)((z + 1) >> 1) : (int)(z >> 1);
}
static void enc_table(enc_t *e, const uint16_t *f, const uint16_t *base, int n) {
    for (int s = 0; s < n; s++) enc_delta(e, (int)f[s] - (int)base[s]);
}
static int dec_table(dec_t *d, uint16_t *f, const uint16_t *base, int n) {
    int32_t sum = 0;
    for (int s = 0; s < n; s++) {
        int v = (int)base[s] + dec_delta(d);
        if (v < 0 || v > (int)TOT) return -1;
        f[s] = (uint16_t)v; sum += v;
    }
    return (sum == (int32_t)TOT && !d->err) ? 0 : -1;
}

/* ------------------------------------------------------------------ symbols */
static inline void len_sym(uint32_t len, int *sym, uint32_t *extra, int *nb) {
    uint32_t L = len - MINLEN;
    if (L < 16) { *sym = 256 + L; *nb = 0; *extra = 0; return; }
    int b = bitlen(L);
    *sym = 272 + (b - 5); *nb = b - 1; *extra = L & ((1u << (b - 1)) - 1);
}
static inline void dist_sym(uint32_t dist, int *sym, uint32_t *extra, int *nb) {
    uint32_t D = dist - 1;
    if (D < 8) { *sym = D; *nb = 0; *extra = 0; return; }
    int b = bitlen(D);
    *sym = 8 + (b - 4) * 4 + ((D >> (b - 3)) & 3); *nb = b - 3; *extra = D & ((1u << (b - 3)) - 1);
}

/* ------------------------------------------------------------------ match finder: RFC 1951 section 4 */
typedef struct { uint32_t len, v; } tok_t;   /* len 0: literal byte v; else a match of `len` bytes at distance v */
typedef struct { int32_t *head, *prev; size_t prev_cap; tok_t *tok; size_t tok_cap;
                 uint32_t *cost, *flen, *fdist; reps_t *frep; tok_t *rev; } mf_t;


static inline uint32_t hash4(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - HBITS);
}
static inline uint32_t match_len(const uint8_t *a, const uint8_t *b, uint32_t lim) {
    uint32_t n = 0;
    while (n + 8 <= lim) {
        uint64_t x, y; memcpy(&x, a + n, 8); memcpy(&y, b + n, 8);
        if (x != y) return n + (__builtin_ctzll(x ^ y) >> 3);
        n += 8;
    }
    while (n < lim && a[n] == b[n]) n++;
    return n;
}
static int mf_reserve(mf_t *m, uint32_t n) {
    if (!m->head && !(m->head = malloc(sizeof(int32_t) << HBITS))) return -1;
    if (m->prev_cap < n) {
        free(m->prev); free(m->tok);
        m->prev = malloc((size_t)n * sizeof *m->prev); m->tok = malloc((size_t)n * sizeof *m->tok);
        if (!m->prev || !m->tok) { m->prev_cap = 0; return -1; }
        m->prev_cap = n;
    }
    return 0;
}
static void mf_free(mf_t *m) {
    free(m->head); free(m->prev); free(m->tok); free(m->cost); free(m->flen); free(m->fdist); free(m->frep); free(m->rev);
    memset(m, 0, sizeof *m);
}

/* Bits saved by a match, in 1/16 bit: each covered byte would cost about 6 bits as a literal; a match costs its
 * length code and extra bits plus its distance code and extra bits (about 2 bits for the repeat distance).
 * Only used to choose between matches; the archive does not depend on these constants beyond that choice. */
static inline int32_t match_gain(uint32_t len, uint32_t dist, const reps_t *R, int32_t lit) {
    int32_t g = (int32_t)len * lit - 64;
    uint32_t L = len - MINLEN;
    if (L >= 16) g -= (bitlen(L) - 1) * 16;
    int k = rep_index(R, dist);
    if (k >= 0) return g - 32 - 8 * k;
    uint32_t D = dist - 1;
    g -= 96;
    if (D >= 8) g -= (bitlen(D) - 3) * 16;
    return g;
}
/* the best earlier match for position p by bits saved (inserting p into its chain); the repeat distance first */
static inline uint32_t mf_find(const uint8_t *d, uint32_t n, uint32_t p, mf_t *m, int depth, uint32_t nice,
                               const reps_t *R, int32_t lit, uint32_t *dist, int32_t *gain) {
    if (p + MINLEN > n) return 0;
    uint32_t h = hash4(d + p), lim = n - p, bestl = 0, seen = MINLEN - 1;
    int32_t bestg = 0;
    if (lim > MAXLEN) lim = MAXLEN;
    for (int k = 0; k < 3; k++) {
        uint32_t r = R->r[k];
        if (!r || r > p) continue;
        uint32_t l = match_len(d + p - r, d + p, lim);
        if (l < MINLEN) continue;
        int32_t g = match_gain(l, r, R, lit);
        if (g > bestg) { bestl = l; *dist = r; bestg = g; }
        if (l > seen) seen = l;
    }
    int32_t c = m->head[h];
    m->prev[p] = c; m->head[h] = (int32_t)p;
    if (seen < nice)
        while (c >= 0 && depth--) {
            if (seen < lim && d[c + seen] == d[p + seen]) {   /* 0.3.2: never read past the block's end (the archive is unchanged) */
                uint32_t l = match_len(d + c, d + p, lim);
                if (l > seen) {
                    seen = l;
                    int32_t g = match_gain(l, p - (uint32_t)c, R, lit);
                    if (g > bestg) { bestg = g; bestl = l; *dist = p - (uint32_t)c; }
                    if (l >= nice) break;
                }
            }
            c = m->prev[c];
        }
    *gain = bestg;
    return bestg > 0 ? bestl : 0;
}
static inline void mf_insert(const uint8_t *d, uint32_t n, uint32_t p, mf_t *m) {
    if (p + MINLEN > n) return;
    uint32_t h = hash4(d + p);
    m->prev[p] = m->head[h]; m->head[h] = (int32_t)p;
}
/* per level: search depth, "good" length (search a quarter as deep after a match this long), the longest match that
 * still gets the lazy look, "nice" length (stop the search). The same kind of settings as zlib's deflate levels. */
typedef struct { int depth; uint32_t good, lazy, nice; } lvl_t;
static const lvl_t LV[10] = {{0, 0, 0, 0}, {4, 4, 0, 16}, {8, 4, 0, 32}, {32, 6, 0, 64}, {16, 4, 16, 32},
                             {32, 8, 32, 64}, {128, 8, 128, 128}, {128, 32, 128, 256}, {128, 32, 128, 256},
                             {128, 32, 128, 256}};   /* levels 8 and 9 search as level 7 (measured, 0.3.0) */
static size_t mf_parse(const uint8_t *d, uint32_t n, int level, mf_t *m) {
    for (uint32_t i = 0; i < (1u << HBITS); i++) m->head[i] = -1;
    const lvl_t *v = &LV[level];
    size_t nt = 0; uint32_t i = 0; reps_t R = {{0, 0, 0}};
    /* the real cost of a literal in this block: its byte entropy (order 0), in 1/16 bit, kept within 2..8 bits */
    int32_t lit = 128;
    if (n) {
        uint32_t h[256] = {0};
        for (uint32_t k = 0; k < n; k++) h[d[k]]++;
        uint64_t sum = 0; uint32_t ln = ilog2fx(n);
        for (int k = 0; k < 256; k++) if (h[k]) sum += (uint64_t)h[k] * (ln - ilog2fx(h[k]));
        lit = (int32_t)((sum / n) >> 12);
        if (lit < 32) lit = 32;
        if (lit > 128) lit = 128;
    }
    while (i < n) {
        uint32_t D = 0; int32_t G = 0;
        uint32_t L = mf_find(d, n, i, m, v->depth, v->nice, &R, lit, &D, &G);
        if (!L) { m->tok[nt].len = 0; m->tok[nt++].v = d[i++]; continue; }
        uint32_t ins = i + 1;                          /* the next position not yet in the chains */
        while (L < v->lazy && i + 1 < n) {             /* one-step lazy matching (RFC 1951 section 4), by bits saved */
            uint32_t D2 = 0; int32_t G2 = 0;
            uint32_t L2 = mf_find(d, n, i + 1, m, L >= v->good ? v->depth >> 2 : v->depth, v->nice, &R, lit, &D2, &G2);
            ins = i + 2;
            if (L2 && G2 > G + 16) { m->tok[nt].len = 0; m->tok[nt++].v = d[i++]; L = L2; D = D2; G = G2; }
            else break;
        }
        m->tok[nt].len = L; m->tok[nt++].v = D; rep_use(&R, D);
        uint32_t end = i + L;
        for (uint32_t q = ins; q < end; q++) mf_insert(d, n, q, m);
        i = end;
    }
    return nt;
}

/* ------------------------------------------------------------------ optimal parse (shortest path, Schuegraf & Heaps 1974)
 * Every way to cut a segment into literals and matches is a path; each step is priced in bits by the file's base
 * tables (from pass 1); the cheapest path is kept. Long matches (>= nice) are taken at once, as zstd and LZMA do. */
#define SEG (1u << 18)
static int OPT_DEPTH[10] = {0, 0, 0, 0, 0, 0, 8, 16, 16, 16};   /* 8 and 9 = 7: deeper search measured +0.05 points at 5x the time */   /* chain depth per position in the cheapest-path search */
#define PLEN 260   /* lengths 4..259 priced from a ready table */
typedef struct { const uint8_t *grp; uint16_t ll[MAXG][NLL]; uint16_t d[NDIST]; uint16_t len[MAXG][PLEN]; } price_t;
static void make_prices(price_t *pr, const uint8_t *grp, int G, const uint16_t (*bl)[NLL], const uint16_t *bd) {
    pr->grp = grp;
    for (int g = 0; g < G; g++)
        for (int s = 0; s < NLL; s++) pr->ll[g][s] = (uint16_t)(bl[g][s] ? 192 - (ilog2fx(bl[g][s]) >> 12) : 256);
    for (int s = 0; s < NDIST; s++) pr->d[s] = (uint16_t)(bd[s] ? 192 - (ilog2fx(bd[s]) >> 12) : 256);
    for (int g = 0; g < G; g++)
        for (uint32_t L = MINLEN; L < PLEN; L++) {
            int s, nb; uint32_t ex; len_sym(L, &s, &ex, &nb);
            pr->len[g][L] = (uint16_t)(pr->ll[g][s] + 16 * nb);
        }
}
static inline uint32_t len_price(const price_t *pr, int g, uint32_t len) {
    if (len < PLEN) return pr->len[g][len];
    int s, nb; uint32_t ex; len_sym(len, &s, &ex, &nb);
    return pr->ll[g][s] + 16u * nb;
}
static inline uint32_t dist_price(const price_t *pr, uint32_t dist, const reps_t *R) {
    int k = rep_index(R, dist);
    if (k >= 0) return pr->d[REP + k];
    int s, nb; uint32_t ex; dist_sym(dist, &s, &ex, &nb);
    return pr->d[s] + 16u * nb;
}
static int opt_reserve(mf_t *m) {
    if (m->cost) return 0;
    m->cost = malloc((SEG + 1) * sizeof(uint32_t)); m->flen = malloc((SEG + 1) * sizeof(uint32_t));
    m->fdist = malloc((SEG + 1) * sizeof(uint32_t)); m->frep = malloc((SEG + 1) * sizeof(reps_t));
    m->rev = malloc((SEG + 1) * sizeof(tok_t));
    return (m->cost && m->flen && m->fdist && m->frep && m->rev) ? 0 : -1;
}
static size_t opt_parse(const uint8_t *d, uint32_t n, int level, mf_t *m, const price_t *pr) {
    for (uint32_t i = 0; i < (1u << HBITS); i++) m->head[i] = -1;
    const lvl_t *v = &LV[level];
    uint32_t nice = v->nice < 64 ? 64 : v->nice;
    size_t nt = 0; uint32_t pos = 0; reps_t rep = {{0, 0, 0}};
    while (pos < n) {
        uint32_t s0 = pos, sl = n - pos < SEG ? n - pos : SEG, cut = sl, longL = 0, longD = 0;
        m->cost[0] = 0; m->frep[0] = rep;
        uint32_t fr = 0;                                   /* cost[0..fr] are set; the rest is set when first reached */
#define REACH(X) do { uint32_t x_ = (X); while (fr < x_) m->cost[++fr] = UINT32_MAX; } while (0)
        for (uint32_t j = 0; j < sl; j++) {
            REACH(j + 1);
            uint32_t p = s0 + j, cj = m->cost[j]; reps_t r = m->frep[j];
            int g = pr->grp[p ? d[p - 1] : 0];
            uint32_t c = cj + pr->ll[g][d[p]];
            if (c < m->cost[j + 1]) { m->cost[j + 1] = c; m->flen[j + 1] = 0; m->frep[j + 1] = r; }
            if (p + MINLEN > n) continue;
            uint32_t lim = sl - j, seen = MINLEN - 1, bestD = 0;
            if (lim > MAXLEN) lim = MAXLEN;
            if (lim < MINLEN) { mf_insert(d, n, p, m); continue; }
#define RELAX(A, B, DIST) do { \
                uint32_t dist_ = (DIST), dp_ = cj + dist_price(pr, dist_, &r); reps_t nr_ = r; rep_use(&nr_, dist_); REACH(j + (B)); \
                for (uint32_t L_ = (A); L_ <= (B); L_++) { \
                    if (L_ > 64 && L_ != (B)) { L_ = (B) - 1; continue; } \
                    uint32_t c_ = dp_ + len_price(pr, g, L_); \
                    if (c_ < m->cost[j + L_]) { m->cost[j + L_] = c_; m->flen[j + L_] = L_; m->fdist[j + L_] = dist_; m->frep[j + L_] = nr_; } \
                } } while (0)
            for (int k = 0; k < 3; k++) {             /* the remembered distances first (R0 is the cheapest code) */
                uint32_t rr = r.r[k];
                if (!rr || rr > p) continue;
                uint32_t l = match_len(d + p - rr, d + p, lim);
                if (l >= MINLEN && l > seen) { RELAX(seen + 1 > MINLEN ? seen + 1 : MINLEN, l, rr); seen = l; bestD = rr; }
            }
            uint32_t h = hash4(d + p); int32_t cc = m->head[h]; int dep = OPT_DEPTH[level];
            m->prev[p] = cc; m->head[h] = (int32_t)p;
            while (seen < nice && seen < lim && cc >= 0 && dep--) {
                if (d[cc + seen] == d[p + seen]) {
                    uint32_t l = match_len(d + cc, d + p, lim);
                    if (l > seen) { RELAX(seen + 1, l, p - (uint32_t)cc); seen = l; bestD = p - (uint32_t)cc; }
                }
                cc = m->prev[cc];
            }
#undef RELAX
            if (seen >= nice) { cut = j; longL = seen; longD = bestD; break; }   /* a long match: take it now */
        }
#undef REACH
        /* the cheapest path to `cut`, read backwards */
        size_t nr = 0; uint32_t j = cut;
        while (j > 0) {
            if (!m->flen[j]) { m->rev[nr].len = 0; m->rev[nr++].v = d[s0 + j - 1]; j--; }
            else { m->rev[nr].len = m->flen[j]; m->rev[nr++].v = m->fdist[j]; j -= m->flen[j]; }
        }
        while (nr) m->tok[nt++] = m->rev[--nr];
        rep = m->frep[cut];
        if (longL) {
            m->tok[nt].len = longL; m->tok[nt++].v = longD; rep_use(&rep, longD);
            uint32_t p = s0 + cut;
            for (uint32_t q = p + 1; q < p + longL; q++) mf_insert(d, n, q, m);
            pos = p + longL;
        } else pos = s0 + sl;
    }
    return nt;
}

/* ------------------------------------------------------------------ lanes */
static int lanes_for(uint32_t blen, int L) {
    uint32_t k = blen / 16384;
    if (k < 1) k = 1;
    return (int)(k < (uint32_t)L ? k : (uint32_t)L);
}

/* ------------------------------------------------------------------ the pack */
typedef struct {
    const uint8_t *src; size_t n; uint32_t bs; int L, level, G;
    uint8_t grp[256];
    uint16_t base_ll[MAXG][NLL], base_d[NDIST];
    uint32_t nblocks;
    buf_t *bout;                 /* one output per block */
    uint32_t *crc;
    uint32_t (*hist)[NLL];       /* pass 1: [threads][256 contexts][NLL] */
    uint32_t (*hist_d)[NDIST];
    tok_t **tok; size_t *ntok; int keep;    /* pass 1 tokens kept for pass 2 (files up to 256 MiB) */
    int opt; price_t *price;                /* pass 2 re-parses by the cheapest path (level >= 6) */
    int enc_pass;                           /* 2, or 3 when the cheapest path is searched twice (level >= 7) */
    volatile uint32_t next; int pass, err;
} pack_t;

static void count_tokens(const tok_t *t, size_t nt, const uint8_t *d, uint32_t *ctx_ll /* [256][NLL] */,
                         uint32_t *dh, const uint8_t *grp, int by_group) {
    uint8_t prev = 0; uint32_t pos = 0; reps_t R = {{0, 0, 0}};
    for (size_t i = 0; i < nt; i++) {
        int c = by_group ? grp[prev] : prev;
        if (!t[i].len) { ctx_ll[c * NLL + t[i].v]++; prev = (uint8_t)t[i].v; pos++; continue; }
        int s, nb; uint32_t ex;
        len_sym(t[i].len, &s, &ex, &nb); ctx_ll[c * NLL + s]++;
        int k = rep_index(&R, t[i].v);
        if (k >= 0) dh[REP + k]++; else { dist_sym(t[i].v, &s, &ex, &nb); dh[s]++; }
        rep_use(&R, t[i].v);
        pos += t[i].len; prev = d[pos - 1];
    }
}

static void pack_block(pack_t *P, uint32_t bi, mf_t *m, int tid) {
    const uint8_t *blk = P->src + (size_t)bi * P->bs;
    uint32_t blen = (uint32_t)((size_t)bi * P->bs + P->bs <= P->n ? P->bs : P->n - (size_t)bi * P->bs);
    int nl = lanes_for(blen, P->L);
    size_t nt; const tok_t *tk;
    if (P->pass == P->enc_pass && P->keep) { tk = P->tok[bi]; nt = P->ntok[bi]; }    /* tokens kept from pass 1 */
    else {
        if (mf_reserve(m, blen ? blen : 1)) { P->err = XOLA_E_MEM; return; }
        if (P->pass >= 2 && P->opt) {
            if (opt_reserve(m)) { P->err = XOLA_E_MEM; return; }
            nt = opt_parse(blk, blen, P->level, m, P->price);
        } else nt = mf_parse(blk, blen, P->level, m);
        tk = m->tok;
    }
    if (P->pass < P->enc_pass) {                     /* a counting pass: statistics of the whole file */
        count_tokens(tk, nt, blk, &P->hist[(size_t)tid * 256][0], P->hist_d[tid], NULL, 0);
        if (P->keep && P->pass == 1) {
            P->tok[bi] = malloc((nt ? nt : 1) * sizeof(tok_t));
            if (!P->tok[bi]) { P->err = XOLA_E_MEM; return; }
            memcpy(P->tok[bi], tk, nt * sizeof(tok_t)); P->ntok[bi] = nt;
        }
        return;
    }
    /* pass 2: count by group, build this block's tables, code; token i goes to lane i mod nl. A block that this does not
     * shrink is coded again as plain bytes only, with every table flat (each byte value 1/256 of the range: exactly 8 bits a
     * byte) - the same coder and the same format, never a block stored outside the coder (one method for every block) */
    for (int flat = 0; flat < 2; flat++) {
    uint32_t cll[MAXG * NLL], cd[NDIST];
    memset(cll, 0, sizeof(uint32_t) * P->G * NLL); memset(cd, 0, sizeof cd);
    if (!flat) count_tokens(tk, nt, blk, cll, cd, P->grp, 1);
    else for (int g = 0; g < P->G; g++) for (int s = 0; s < 256; s++) cll[g * NLL + s] = 1;
    {
        tab_t tl[MAXG], td;
        enc_t te; enc_init(&te);
        for (int g = 0; g < P->G; g++) {
            normalize(cll + g * NLL, NLL, tl[g].f, P->base_ll[g]);
            enc_table(&te, tl[g].f, P->base_ll[g], NLL); cumulate(&tl[g], NLL);
        }
        normalize(cd, NDIST, td.f, P->base_d);
        enc_table(&te, td.f, P->base_d, NDIST); cumulate(&td, NDIST);
        enc_flush(&te);
        enc_t le[MAXLANES];
        for (int k = 0; k < nl; k++) enc_init(&le[k]);
        uint8_t prev = 0; uint32_t pos = 0; reps_t R = {{0, 0, 0}};
        if (flat) for (uint32_t i = 0; i < blen; i++) {     /* plain bytes only */
            const tab_t *t = &tl[P->grp[prev]]; int sy = blk[i];
            enc_sym(&le[i % nl], t->c[sy], t->f[sy]); prev = (uint8_t)sy;
        }
        for (size_t i = 0; i < (flat ? 0 : nt); i++) {
            enc_t *e = &le[i % nl];
            const tab_t *t = &tl[P->grp[prev]];
            if (!tk[i].len) { int s = tk[i].v; enc_sym(e, t->c[s], t->f[s]); prev = (uint8_t)s; pos++; continue; }
            int s, nb; uint32_t ex;
            len_sym(tk[i].len, &s, &ex, &nb); enc_sym(e, t->c[s], t->f[s]); enc_bits(e, ex, nb);
            int k = rep_index(&R, tk[i].v);
            if (k >= 0) enc_sym(e, td.c[REP + k], td.f[REP + k]);
            else { dist_sym(tk[i].v, &s, &ex, &nb); enc_sym(e, td.c[s], td.f[s]); enc_bits(e, ex, nb); }
            rep_use(&R, tk[i].v);
            pos += tk[i].len; prev = blk[pos - 1];
        }
        for (int k = 0; k < nl; k++) enc_flush(&le[k]);
#ifdef XOLA_STATS
        {   /* where the bits go (analysis build only) */
            double bl = 0, bm = 0, bd = 0, bx = 0; size_t nlit = 0, nmat = 0, nrep = 0, mlen = 0; uint8_t pv = 0; uint32_t ps = 0, rp = 0;
            for (size_t i = 0; i < nt; i++) {
                const tab_t *t = &tl[P->grp[pv]];
                if (!tk[i].len) { bl += 12 - __builtin_log2((double)t->f[tk[i].v]); nlit++; pv = (uint8_t)tk[i].v; ps++; continue; }
                int s2, nb; uint32_t ex; len_sym(tk[i].len, &s2, &ex, &nb); bm += 12 - __builtin_log2((double)t->f[s2]); bx += nb;
                if (0) { (void)rp; }
                else { dist_sym(tk[i].v, &s2, &ex, &nb); bd += 12 - __builtin_log2((double)td.f[s2]); bx += nb; }
                rp = tk[i].v; nmat++; mlen += tk[i].len; ps += tk[i].len; pv = blk[ps - 1];
            }
            fprintf(stderr, "STATS block %u: %u bytes | literals %zu (%.0f B, %.2f bits each) | matches %zu (rep %zu, avg len %.1f) "
                    "len %.0f B, dist %.0f B, extra bits %.0f B | tables %zu B\n", bi, blen, nlit, bl / 8, nlit ? bl / nlit : 0, nmat, nrep,
                    nmat ? (double)mlen / nmat : 0, bm / 8, bd / 8, bx / 8, te.out.n);
        }
#endif
        buf_t *o = &P->bout[bi]; uint8_t w[4];
        size_t coded = 1 + 4 + te.out.n + 4 * (nl - 1);
        for (int k = 0; k < nl; k++) coded += le[k].out.n;
        int bad = te.out.err;
        for (int k = 0; k < nl; k++) bad |= le[k].out.err;
        int again = !bad && !flat && coded >= (size_t)blen + 1;   /* not shrunk: code it again as plain bytes */
        if (bad) P->err = XOLA_E_MEM;
        else if (!again) {
            bput1(o, 1); put32(w, (uint32_t)te.out.n); bput(o, w, 4); bput(o, te.out.p, te.out.n);
            for (int k = 0; k + 1 < nl; k++) { put32(w, (uint32_t)le[k].out.n); bput(o, w, 4); }
            for (int k = 0; k < nl; k++) bput(o, le[k].out.p, le[k].out.n);
        }
        if (o->err) P->err = XOLA_E_MEM;
        free(te.out.p);
        for (int k = 0; k < nl; k++) free(le[k].out.p);
        if (!again) break;
    }
    }
    P->crc[bi] = xola_crc32c(0, blk, blen);
    if (P->keep) { free(P->tok[bi]); P->tok[bi] = NULL; }
}

typedef struct { pack_t *P; int tid; } work_t;
static void *pack_worker(void *arg) {
    work_t *w = arg; mf_t m; memset(&m, 0, sizeof m);
    for (;;) {
        uint32_t bi = __atomic_fetch_add(&w->P->next, 1, __ATOMIC_RELAXED);
        if (bi >= w->P->nblocks || w->P->err) break;
        pack_block(w->P, bi, &m, w->tid);
    }
    mf_free(&m);
    return NULL;
}
static int run_workers(void *(*fn)(void *), void *P, int threads, size_t stride, void *works) {
    pthread_t th[64]; int started = 0;
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    for (int t = 1; t < threads; t++)
        if (!pthread_create(&th[t], NULL, fn, (char *)works + stride * t)) started = t;
        else break;
    fn(works);
    for (int t = 1; t <= started; t++) pthread_join(th[t], NULL);
    (void)P;
    return 0;
}

/* group the 256 order-1 contexts into G groups by their statistics (encoder only; the decoder reads the map) */
static void group_contexts(const uint32_t (*H)[NLL], int G, uint8_t *grp) {
    uint64_t tot[256];
    for (int c = 0; c < 256; c++) { tot[c] = 0; for (int s = 0; s < NLL; s++) tot[c] += H[c][s]; }
    memset(grp, 0, 256);
    if (G <= 1) return;
    int seed[MAXG], used[256] = {0};
    for (int g = 0; g < G; g++) {               /* seeds: the G busiest contexts (lowest number on a tie) */
        int b = -1;
        for (int c = 0; c < 256; c++) if (!used[c] && (b < 0 || tot[c] > tot[b])) b = c;
        seed[g] = b; used[b] = 1;
    }
    for (int c = 0; c < 256; c++) grp[c] = 0;
    for (int g = 0; g < G; g++) grp[seed[g]] = (uint8_t)g;
    uint64_t (*Q)[NLL] = malloc(sizeof(uint64_t) * MAXG * NLL);
    uint32_t LQ[MAXG][NLL], LT[MAXG];
    if (!Q) return;                       /* out of memory: everything stays in group 0, still a valid archive */
    for (int round = 0; round < 6; round++) {
        memset(Q, 0, sizeof(uint64_t) * MAXG * NLL);
        for (int c = 0; c < 256; c++) if (tot[c]) for (int s = 0; s < NLL; s++) Q[grp[c]][s] += H[c][s];
        for (int g = 0; g < G; g++) {
            uint64_t t = 0;
            int empty = 1;
            for (int s = 0; s < NLL; s++) if (Q[g][s]) { empty = 0; break; }
            if (empty) for (int s = 0; s < NLL; s++) Q[g][s] = H[seed[g]][s];
            for (int s = 0; s < NLL; s++) { t += Q[g][s] + 1; LQ[g][s] = ilog2fx(Q[g][s] + 1); }
            LT[g] = ilog2fx(t);
        }
        int moved = 0;
        for (int c = 0; c < 256; c++) {
            if (!tot[c]) { grp[c] = 0; continue; }
            uint64_t bestc = ~0ull; int bg = 0;
            for (int g = 0; g < G; g++) {
                uint64_t cost = 0;
                for (int s = 0; s < NLL; s++) if (H[c][s]) cost += (uint64_t)H[c][s] * (LT[g] - LQ[g][s]);
                if (cost < bestc) { bestc = cost; bg = g; }
            }
            if (grp[c] != bg) { grp[c] = (uint8_t)bg; moved = 1; }
        }
        if (!moved) break;
    }
    free(Q);
}

/* the file's statistics -> context groups, base tables, prices. Returns 0, or -1 when out of memory. */
static int file_stats(pack_t *P, int T) {
    uint32_t (*H)[NLL] = calloc(256, sizeof *H); uint32_t Hd[NDIST] = {0};
    if (!H) return -1;
    for (int t = 0; t < T; t++) {
        for (int c = 0; c < 256; c++) for (int s = 0; s < NLL; s++) H[c][s] += P->hist[(size_t)t * 256 + c][s];
        for (int s = 0; s < NDIST; s++) Hd[s] += P->hist_d[t][s];
    }
    group_contexts((const uint32_t (*)[NLL])H, P->G, P->grp);
    uint32_t Q[NLL];
    for (int g = 0; g < P->G; g++) {
        memset(Q, 0, sizeof Q);
        for (int c = 0; c < 256; c++) if (P->grp[c] == g) for (int s = 0; s < NLL; s++) Q[s] += H[c][s];
        normalize(Q, NLL, P->base_ll[g], ONE);
    }
    normalize(Hd, NDIST, P->base_d, ONE);
    free(H);
    if (P->opt) {
        if (!P->price && !(P->price = malloc(sizeof *P->price))) return -1;
        make_prices(P->price, P->grp, P->G, (const uint16_t (*)[NLL])P->base_ll, P->base_d);
    }
    return 0;
}

void xola_default_params(xola_params *p) { p->block_kb = 4096; p->lanes = 4; p->threads = 1; p->level = 6; }

int xola_pack(const uint8_t *src, size_t n, const xola_params *pp, uint8_t **out, size_t *out_n) {
    xola_params p; if (pp) p = *pp; else xola_default_params(&p);
    if (p.block_kb < 64 || p.block_kb > 16384 || p.lanes < 1 || p.lanes > MAXLANES || p.level < 1 || p.level > 9) return XOLA_E_ARG;
    pthread_once(&flat_once, flat_init);
    pack_t *P = calloc(1, sizeof *P);
    if (!P) return XOLA_E_MEM;
    P->src = src; P->n = n; P->bs = p.block_kb * 1024; P->L = p.lanes; P->level = p.level;
    P->G = n < 65536 ? 1 : n < (1u << 20) ? 4 : n < (16u << 20) ? 8 : 16;
    P->nblocks = (uint32_t)((n + P->bs - 1) / P->bs);
    int T = p.threads < 1 ? 1 : p.threads > 64 ? 64 : p.threads;
    if (T > (int)P->nblocks && P->nblocks) T = (int)P->nblocks;
    if (!P->nblocks) T = 1;
    P->bout = calloc(P->nblocks ? P->nblocks : 1, sizeof(buf_t));
    P->crc = calloc(P->nblocks ? P->nblocks : 1, sizeof(uint32_t));
    P->hist = calloc((size_t)T * 256, sizeof *P->hist);
    P->hist_d = calloc((size_t)T, sizeof *P->hist_d);
    work_t *W = calloc((size_t)T, sizeof *W);
    P->opt = p.level >= 6;
    P->enc_pass = p.level >= 7 ? 3 : 2;
    P->keep = !P->opt && n <= ((size_t)256 << 20);
    P->tok = calloc(P->nblocks ? P->nblocks : 1, sizeof *P->tok);
    P->ntok = calloc(P->nblocks ? P->nblocks : 1, sizeof *P->ntok);
    int rc = XOLA_E_MEM;
    if (!P->tok || !P->ntok || !P->bout || !P->crc || !P->hist || !P->hist_d || !W) goto done;
    for (int t = 0; t < T; t++) { W[t].P = P; W[t].tid = t; }
    /* pass 1: statistics of the whole file */
    P->pass = 1; P->next = 0;
    run_workers(pack_worker, P, T, sizeof *W, W);
    if (P->err) { rc = P->err; goto done; }
    if (file_stats(P, T)) goto done;
    if (P->enc_pass == 3) {                         /* the cheapest path again, priced by the first one's choices */
        memset(P->hist, 0, (size_t)T * 256 * sizeof *P->hist); memset(P->hist_d, 0, (size_t)T * sizeof *P->hist_d);
        P->pass = 2; P->next = 0;
        run_workers(pack_worker, P, T, sizeof *W, W);
        if (P->err) { rc = P->err; goto done; }
        if (file_stats(P, T)) goto done;
    }
    /* the coding pass */
    P->pass = P->enc_pass; P->next = 0;
    run_workers(pack_worker, P, T, sizeof *W, W);
    if (P->err) { rc = P->err; goto done; }
    {
        buf_t o = {0}; uint8_t w[8];
        enc_t be; enc_init(&be);
        int mb = bitlen((uint32_t)P->G - 1);                       /* the context map: bitlen(G-1) bits each */
        for (int c = 0; c < 256; c++) enc_bits(&be, P->grp[c], mb);
        for (int g = 0; g < P->G; g++) enc_table(&be, P->base_ll[g], ZERO, NLL);
        enc_table(&be, P->base_d, ZERO, NDIST);
        enc_flush(&be);
        bput(&o, MAGIC, 4); bput1(&o, 3); bput1(&o, (uint8_t)P->G); bput1(&o, (uint8_t)P->L); bput1(&o, (uint8_t)P->level);
        put32(w, P->bs); bput(&o, w, 4); put64(w, n); bput(&o, w, 8);
        put32(w, (uint32_t)be.out.n); bput(&o, w, 4); bput(&o, be.out.p, be.out.n);
        free(be.out.p);
        size_t hlen = o.n;
        for (uint32_t b = 0; b < P->nblocks; b++) bput(&o, P->bout[b].p, P->bout[b].n);
        size_t ioff = o.n;
        for (uint32_t b = 0; b < P->nblocks; b++) {
            uint32_t blen = (uint32_t)((size_t)b * P->bs + P->bs <= n ? P->bs : n - (size_t)b * P->bs);
            put32(w, (uint32_t)P->bout[b].n); bput(&o, w, 4);
            put32(w, blen); bput(&o, w, 4);
            put32(w, P->crc[b]); bput(&o, w, 4);
        }
        uint32_t mc = xola_crc32c(0, o.p, hlen);
        mc = xola_crc32c(mc, o.p + ioff, o.n - ioff);
        put64(w, ioff); bput(&o, w, 8); put32(w, P->nblocks); bput(&o, w, 4); put32(w, (uint32_t)hlen); bput(&o, w, 4);
        put32(w, mc); bput(&o, w, 4); bput(&o, MAGIC_END, 8);
        if (o.err) { free(o.p); goto done; }
        *out = o.p; *out_n = o.n; rc = XOLA_OK;
    }
done:
    if (P->bout) for (uint32_t b = 0; b < P->nblocks; b++) free(P->bout[b].p);
    if (P->tok) for (uint32_t b = 0; b < P->nblocks; b++) free(P->tok[b]);
    free(P->tok); free(P->ntok); free(P->price);
    free(P->bout); free(P->crc); free(P->hist); free(P->hist_d); free(W); free(P);
    return rc;
}

/* ------------------------------------------------------------------ the restore */
typedef struct {
    const uint8_t *arc; size_t n;
    uint64_t orig; uint32_t bs, nblocks; int G, L;
    size_t hlen, ioff;
    uint8_t grp[256];
    uint16_t base_ll[MAXG][NLL], base_d[NDIST];
    uint64_t *off;              /* block offsets in the archive */
    const uint8_t *idx;
} arc_t;

static int arc_open(const uint8_t *a, size_t n, arc_t *A) {
    pthread_once(&flat_once, flat_init);
    memset(A, 0, sizeof *A);
    if (n < 24 + FOOT || memcmp(a, MAGIC, 4) || a[4] != 3 || memcmp(a + n - 8, MAGIC_END, 8)) return XOLA_E_FORMAT;
    const uint8_t *f = a + n - FOOT;
    A->arc = a; A->n = n; A->ioff = get64(f); A->nblocks = get32(f + 8); A->hlen = get32(f + 12);
    uint32_t mc = get32(f + 16);
    if (A->hlen < 24 || A->hlen > n || A->ioff < A->hlen || A->ioff > n - FOOT ||
        (uint64_t)A->nblocks * 12 != (uint64_t)(n - FOOT - A->ioff)) return XOLA_E_FORMAT;
    uint32_t c = xola_crc32c(0, a, A->hlen);
    if (xola_crc32c(c, a + A->ioff, n - FOOT - A->ioff) != mc) return XOLA_E_FORMAT;
    A->G = a[5]; A->L = a[6]; A->bs = get32(a + 8); A->orig = get64(a + 12);
    if (A->G < 1 || A->G > MAXG || A->L < 1 || A->L > MAXLANES || A->bs < 65536 || A->bs > (16u << 20)) return XOLA_E_FORMAT;
    if ((A->orig + A->bs - 1) / A->bs != A->nblocks) return XOLA_E_FORMAT;
    uint32_t bl = get32(a + 20);
    if (24 + (uint64_t)bl != A->hlen) return XOLA_E_FORMAT;
    dec_t d; dec_init(&d, a + 24, bl);
    int mb = bitlen((uint32_t)A->G - 1);
    for (int c = 0; c < 256; c++) { uint32_t g = dec_bits(&d, mb); if (g >= (uint32_t)A->G) return XOLA_E_FORMAT; A->grp[c] = (uint8_t)g; }
    for (int g = 0; g < A->G; g++) if (dec_table(&d, A->base_ll[g], ZERO, NLL)) return XOLA_E_FORMAT;
    if (dec_table(&d, A->base_d, ZERO, NDIST)) return XOLA_E_FORMAT;
    A->idx = a + A->ioff;
    A->off = malloc((A->nblocks ? A->nblocks : 1) * sizeof(uint64_t));
    if (!A->off) return XOLA_E_MEM;
    uint64_t o = A->hlen;
    for (uint32_t b = 0; b < A->nblocks; b++) {
        A->off[b] = o; o += get32(A->idx + 12 * b);
        uint32_t blen = get32(A->idx + 12 * b + 4);
        uint64_t want = (uint64_t)b * A->bs + A->bs <= A->orig ? A->bs : A->orig - (uint64_t)b * A->bs;
        if (blen != want) { free(A->off); A->off = NULL; return XOLA_E_FORMAT; }
    }
    if (o != A->ioff) { free(A->off); A->off = NULL; return XOLA_E_FORMAT; }
    return XOLA_OK;
}

/* decode block b into dst (its full length). Returns 0 when the block restored and passed its check. */
static int unpack_block(const arc_t *A, uint32_t b, uint8_t *dst, uint16_t *lutbuf) {
    const uint8_t *p = A->arc + A->off[b];
    uint32_t csz = get32(A->idx + 12 * b), blen = get32(A->idx + 12 * b + 4), crc = get32(A->idx + 12 * b + 8);
    if (csz < 1) return -1;
    if (p[0] == 0) {
        if (csz != blen + 1) return -1;
        memcpy(dst, p + 1, blen);
        return xola_crc32c(0, dst, blen) == crc ? 0 : -1;
    }
    if (p[0] != 1 || csz < 5) return -1;
    const uint8_t *end = p + csz;
    uint32_t tsz = get32(p + 1);
    const uint8_t *q = p + 5;
    if ((uint64_t)tsz > (uint64_t)(end - q)) return -1;
    tab_t tl[MAXG], td; dec_t d;
    dec_init(&d, q, tsz);
    for (int g = 0; g < A->G; g++) { if (dec_table(&d, tl[g].f, A->base_ll[g], NLL)) return -1; cumulate(&tl[g], NLL); }
    if (dec_table(&d, td.f, A->base_d, NDIST)) return -1;
    cumulate(&td, NDIST);
    q += tsz;
    uint16_t *lut_ll[MAXG], *lut_d = lutbuf + (size_t)A->G * TOT;
    for (int g = 0; g < A->G; g++) { lut_ll[g] = lutbuf + (size_t)g * TOT; build_lut(&tl[g], NLL, lut_ll[g]); }
    build_lut(&td, NDIST, lut_d);
    int nl = lanes_for(blen, A->L);
    if ((size_t)(end - q) < 4u * (nl - 1)) return -1;
    uint32_t lsz[MAXLANES]; uint64_t used = 0;
    for (int k = 0; k + 1 < nl; k++) { lsz[k] = get32(q); q += 4; used += lsz[k]; }
    if (used > (uint64_t)(end - q)) return -1;
    lsz[nl - 1] = (uint32_t)((end - q) - used);
    dec_t ld[MAXLANES];
    for (int k = 0; k < nl; k++) { dec_init(&ld[k], q, lsz[k]); q += lsz[k]; }
    uint32_t pos = 0; reps_t R = {{0, 0, 0}}; uint8_t prev = 0; int bad = 0, k = 0;
    while (pos < blen) {                           /* token i comes from lane i mod nl */
        dec_t *D = &ld[k]; if (++k == nl) k = 0;
        int g = A->grp[prev]; const tab_t *t = &tl[g];
        uint32_t r = D->range >> TOTB, f = (D->code - D->low) / r;
        if (f >= TOT) { bad = 1; break; }
        int s = lut_ll[g][f];
        D->low += t->c[s] * r; D->range = r * t->f[s]; dec_norm(D);
        if (s < 256) { dst[pos++] = (uint8_t)s; prev = (uint8_t)s; continue; }
        uint32_t len;
        if (s < 272) len = MINLEN + (s - 256);
        else { int bb = s - 272 + 5; len = MINLEN + ((1u << (bb - 1)) | dec_bits(D, bb - 1)); }
        r = D->range >> TOTB; f = (D->code - D->low) / r;
        if (f >= TOT) { bad = 1; break; }
        int ds = lut_d[f];
        D->low += td.c[ds] * r; D->range = r * td.f[ds]; dec_norm(D);
        uint32_t dist;
        if (ds >= REP) dist = R.r[ds - REP];
        else if (ds < 8) dist = ds + 1;
        else { int bb = (ds - 8) / 4 + 4; uint32_t hi = (1u << (bb - 1)) | ((uint32_t)((ds - 8) & 3) << (bb - 3));
               dist = (hi | dec_bits(D, bb - 3)) + 1; }
        if (D->err || !dist || dist > pos || len > blen - pos) { bad = 1; break; }
        rep_use(&R, dist);
        uint8_t *o = dst + pos; const uint8_t *src = o - dist;
        if (dist >= 8) { uint32_t i = 0; for (; i + 8 <= len; i += 8) memcpy(o + i, src + i, 8); for (; i < len; i++) o[i] = src[i]; }
        else for (uint32_t i = 0; i < len; i++) o[i] = src[i];
        pos += len; prev = dst[pos - 1];
    }
    for (int k = 0; k < nl; k++) if (ld[k].err) bad = 1;
    if (bad) return -1;
    return xola_crc32c(0, dst, blen) == crc ? 0 : -1;
}

typedef struct {
    const arc_t *A; uint8_t *dst; uint32_t b0, b1; uint64_t base_off;
    volatile uint32_t next; uint8_t *damaged; int err;
} un_t;
typedef struct { un_t *U; } unw_t;
static void *unpack_worker(void *arg) {
    un_t *U = ((unw_t *)arg)->U;
    uint16_t *lut = malloc(sizeof(uint16_t) * TOT * (MAXG + 1));
    if (!lut) { U->err = XOLA_E_MEM; return NULL; }
    for (;;) {
        uint32_t b = U->b0 + __atomic_fetch_add(&U->next, 1, __ATOMIC_RELAXED);
        if (b >= U->b1) break;
        uint8_t *dst = U->dst + ((uint64_t)b * U->A->bs - U->base_off);
        uint32_t blen = get32(U->A->idx + 12 * b + 4);
        if (unpack_block(U->A, b, dst, lut)) { memset(dst, 0, blen); U->damaged[b - U->b0] = 1; }
    }
    free(lut);
    return NULL;
}
static int unpack_blocks(const arc_t *A, uint32_t b0, uint32_t b1, int threads, uint8_t *dst, xola_report *rep) {
    un_t U; memset(&U, 0, sizeof U);
    U.A = A; U.dst = dst; U.b0 = b0; U.b1 = b1; U.base_off = (uint64_t)b0 * A->bs;
    U.damaged = calloc(b1 - b0 + 1, 1);
    if (!U.damaged) return XOLA_E_MEM;
    int T = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    if (T > (int)(b1 - b0) && b1 > b0) T = (int)(b1 - b0);
    unw_t W[64];
    for (int t = 0; t < T; t++) W[t].U = &U;
    run_workers(unpack_worker, &U, T, sizeof W[0], W);
    int rc = U.err;
    if (rep) { memset(rep, 0, sizeof *rep); rep->nblocks = A->nblocks; rep->block_size = A->bs; }
    for (uint32_t b = b0; b < b1; b++)
        if (U.damaged[b - b0]) {
            if (rep) { if (rep->ndamaged < 16) rep->first_damaged[rep->ndamaged] = b; rep->ndamaged++; }
            if (!rc) rc = XOLA_E_DAMAGED;
        }
    free(U.damaged);
    return rc;
}

int xola_unpack(const uint8_t *arc, size_t n, int threads, uint8_t **out, size_t *out_n, xola_report *rep) {
    arc_t A; int rc = arc_open(arc, n, &A);
    if (rep) memset(rep, 0, sizeof *rep);
    if (rc) return rc;
    uint8_t *dst = malloc(A.orig ? A.orig : 1);
    if (!dst) { free(A.off); return XOLA_E_MEM; }
    rc = unpack_blocks(&A, 0, A.nblocks, threads, dst, rep);
    free(A.off);
    if (rc && rc != XOLA_E_DAMAGED) { free(dst); return rc; }
    *out = dst; *out_n = (size_t)A.orig;
    return rc;
}

int xola_unpack_range(const uint8_t *arc, size_t n, uint64_t from, uint64_t len, uint8_t **out, size_t *out_n,
                      xola_report *rep) {
    arc_t A; int rc = arc_open(arc, n, &A);
    if (rep) memset(rep, 0, sizeof *rep);
    if (rc) return rc;
    if (from > A.orig) from = A.orig;
    if (len > A.orig - from) len = A.orig - from;
    uint32_t b0 = (uint32_t)(from / A.bs), b1 = len ? (uint32_t)((from + len - 1) / A.bs + 1) : b0;
    uint64_t span = b1 > b0 ? (b1 == A.nblocks ? A.orig : (uint64_t)b1 * A.bs) - (uint64_t)b0 * A.bs : 0;
    uint8_t *tmp = malloc(span ? span : 1), *dst = malloc(len ? len : 1);
    if (!tmp || !dst) { free(tmp); free(dst); free(A.off); return XOLA_E_MEM; }
    rc = b1 > b0 ? unpack_blocks(&A, b0, b1, 1, tmp, rep) : 0;
    if (!rc || rc == XOLA_E_DAMAGED) memcpy(dst, tmp + (from - (uint64_t)b0 * A.bs), len);
    free(tmp); free(A.off);
    if (rc && rc != XOLA_E_DAMAGED) { free(dst); return rc; }
    *out = dst; *out_n = (size_t)len;
    return rc;
}

int xola_info(const uint8_t *arc, size_t n, xola_info_t *info) {
    arc_t A; int rc = arc_open(arc, n, &A);
    if (rc) return rc;
    info->orig_size = A.orig; info->block_size = A.bs; info->nblocks = A.nblocks; info->groups = A.G; info->lanes = A.L;
    free(A.off);
    return XOLA_OK;
}
