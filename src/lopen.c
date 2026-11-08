/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
/* lopen.c - Xign Lite: opening the packed parts inside documents (see lopen.h). 2026-11-08.
 *
 * Stored form of an opened file:
 *   "LOP1" | u32 count | u64 original size | count x { u64 offset in the original, u32 packed length, u32 open length,
 *   u8 level, u8 memLevel, u8 strategy, u8 0 } | the body = the original with every packed part replaced by its open bytes.
 * All numbers little-endian. The parts are in order and never overlap.
 */
#include "lopen.h"
#include "zlib/zlib.h"
#include "ljpeg.h"
#include "lcoef.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAGIC "LOP1"
#define HDR 16
#define REC 20
#define MIN_PACKED 64                 /* smaller parts are not worth a table entry */
#define MAX_OPEN ((size_t)64 << 20)   /* one part opens to at most 64 MB */
#define MAX_TOTAL ((size_t)512 << 20) /* a whole opened file at most 512 MB */
#define GIVE_UP 8                     /* parts in a row that no setting rebuilds: stop trying new settings in this file */


static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int lopen_wanted(const uint8_t *p, size_t n) {
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return 1;
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return 1;
    if (n >= 5 && !memcmp(p, "%PDF-", 5)) return 1;
    if (n >= 4 && !memcmp(p, "PK\3\4", 4)) return 1;
    return 0;
}

/* unpack the raw deflate stream at p (at most n bytes): its open bytes and how many packed bytes it used */
static int inflate_at(const uint8_t *p, size_t n, uint8_t **o, size_t *on, size_t *used) {
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    size_t cap = 1 << 16, len = 0; uint8_t *b = malloc(cap);
    if (!b) { inflateEnd(&z); return -1; }
    z.next_in = (Bytef *)p; z.avail_in = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uInt)n;
    int rc;
    for (;;) {
        if (len == cap) {
            if (cap >= MAX_OPEN) { rc = Z_BUF_ERROR; break; }
            size_t c = cap * 2; uint8_t *q = realloc(b, c);
            if (!q) { free(b); inflateEnd(&z); return -1; }
            b = q; cap = c;
        }
        z.next_out = b + len; z.avail_out = (uInt)(cap - len);
        rc = inflate(&z, Z_NO_FLUSH);
        len = cap - z.avail_out;
        if (rc == Z_STREAM_END || (rc != Z_OK && rc != Z_BUF_ERROR)) break;
        if (rc == Z_BUF_ERROR && z.avail_in == 0) break;
    }
    *used = (size_t)z.total_in; inflateEnd(&z);
    if (rc != Z_STREAM_END) { free(b); return 1; }
    *o = b; *on = len; return 0;
}

/* pack the open bytes with one setting and compare with the packed bytes as they come out: 1 = the same bytes */
static int same_again_w(const uint8_t *raw, size_t rn, const uint8_t *packed, size_t pn, int lv, int mem, int st, int wbits) {
    z_stream z; memset(&z, 0, sizeof z);
    if (deflateInit2(&z, lv, Z_DEFLATED, wbits, mem, st) != Z_OK) return 0;
    uint8_t buf[16384]; size_t got = 0; int same = 1, rc;
    z.next_in = (Bytef *)raw; z.avail_in = (uInt)rn;
    do {
        z.next_out = buf; z.avail_out = sizeof buf;
        rc = deflate(&z, Z_FINISH);
        size_t k = sizeof buf - z.avail_out;
        if (got + k > pn || memcmp(buf, packed + got, k)) { same = 0; break; }
        got += k;
    } while (rc == Z_OK);
    if (same && (rc != Z_STREAM_END || got != pn)) same = 0;
    deflateEnd(&z);
    return same;
}
static int same_again(const uint8_t *raw, size_t rn, const uint8_t *packed, size_t pn, int lv, int mem, int st) {
    return same_again_w(raw, rn, packed, pn, lv, mem, st, -15);
}

/* the settings tried, the common ones first */
static const uint8_t SET[][3] = {
    {6, 8, 0}, {9, 8, 0}, {1, 8, 0}, {5, 8, 0}, {4, 8, 0}, {3, 8, 0}, {2, 8, 0}, {7, 8, 0}, {8, 8, 0},
    {6, 9, 0}, {9, 9, 0}, {1, 9, 0}, {5, 9, 0}, {4, 9, 0}, {3, 9, 0}, {2, 9, 0}, {7, 9, 0}, {8, 9, 0},
    {6, 8, 1}, {9, 8, 1}, {1, 8, 1}, {5, 8, 1}, {4, 8, 1}, {3, 8, 1}, {2, 8, 1}, {7, 8, 1}, {8, 8, 1},
    {6, 9, 1}, {9, 9, 1}, {1, 9, 1}, {5, 9, 1}, {4, 9, 1}, {3, 9, 1}, {2, 9, 1}, {7, 9, 1}, {8, 9, 1},
};
#define NSET (sizeof SET / sizeof *SET)

/* ---------------------------------------------------------------- finding the packed parts (one pass, in order) */
typedef struct { uint64_t off; uint32_t used; size_t known; uint8_t *raw; size_t rn; int hit; } cand_t;
typedef struct { cand_t *c; size_t n, cap; uint64_t end_last; size_t total_open; } cands_t;

/* unpack the stream at p[off]; kept as a candidate when it is a whole deflate stream of a useful size */
static int candidate(cands_t *C, const uint8_t *p, size_t n, size_t off, size_t known) {
    if (off < C->end_last || off >= n) return 0;
    size_t lim = known ? known : n - off;
    if (off + lim > n) return 0;
    uint8_t *raw = NULL; size_t rn = 0, used = 0;
    int r = inflate_at(p + off, lim, &raw, &rn, &used);
    if (r < 0) return -1;
    if (r) return 0;
    if (used < MIN_PACKED || (known && used != known) || used > 0xFFFFFFFFu || rn > MAX_OPEN || C->total_open + rn > MAX_TOTAL) { free(raw); return 0; }
    if (C->n == C->cap) { size_t k = C->cap ? C->cap * 2 : 64; cand_t *q = realloc(C->c, k * sizeof *q); if (!q) { free(raw); return -1; } C->c = q; C->cap = k; }
    cand_t *t = &C->c[C->n++]; t->off = off; t->used = (uint32_t)used; t->known = known; t->raw = raw; t->rn = rn; t->hit = -1;
    C->end_last = off + used; C->total_open += rn;
    return 0;
}

static void scan_zip(cands_t *C, const uint8_t *p, size_t n, int *oom) {
    for (size_t i = 0; i + 30 <= n && !*oom; i++) {
        if (p[i] != 'P' || memcmp(p + i, "PK\3\4", 4)) continue;
        uint16_t flags = get16(p + i + 6), method = get16(p + i + 8), nl = get16(p + i + 26), el = get16(p + i + 28);
        uint32_t cs = get32(p + i + 18);
        size_t data = i + 30 + (size_t)nl + el;
        if (method != 8 || (flags & 1) || data >= n) continue;              /* deflate only, never an encrypted part */
        size_t known = (flags & 8) ? 0 : cs;                               /* size in a data descriptor: found by inflating */
        if (candidate(C, p, n, data, known) < 0) *oom = 1;
        if (C->n && C->end_last > i) i = (size_t)C->end_last - 1;
    }
}

static void scan_pdf(cands_t *C, const uint8_t *p, size_t n, int *oom) {
    for (size_t i = 0; i + 8 <= n && !*oom; i++) {
        if (p[i] != 's' || memcmp(p + i, "stream", 6)) continue;
        if (i >= 3 && !memcmp(p + i - 3, "end", 3)) continue;             /* "endstream" */
        size_t s = i + 6;
        if (s < n && p[s] == '\r') s++;
        if (s < n && p[s] == '\n') s++; else if (s == i + 6) continue;
        if (s + 2 >= n) continue;
        uint8_t cmf = p[s], flg = p[s + 1];
        if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 || (flg & 32)) continue;   /* a zlib header, no dictionary */
        if (candidate(C, p, n, s + 2, 0) < 0) *oom = 1;
        if (C->n && C->end_last > i) i = (size_t)C->end_last - 1;
    }
}

/* ---------------------------------------------------------------- trying the settings, on all cores
 * The candidates go in rounds of 64, in file order (a fixed size, so the archive is the same on any number of cores). Inside a round every part first tries the settings that
 * already worked in this file, then (unless the file gave up) all the others. Between rounds the worked settings are
 * shared and the give-up rule is applied: after GIVE_UP parts in a row that no setting rebuilds, only worked settings. */
typedef struct {
    const uint8_t *p; cand_t *c; size_t a, b; int next; pthread_mutex_t mu;
    int good[NSET]; int ngood; int stop_new;
} trial_t;

static int pick(trial_t *T) { pthread_mutex_lock(&T->mu); int i = T->next < (int)(T->b - T->a) ? T->next++ : -1; pthread_mutex_unlock(&T->mu); return i; }

static void *trial_worker(void *arg) {
    trial_t *T = arg; int i;
    while ((i = pick(T)) >= 0) {
        cand_t *q = &T->c[T->a + (size_t)i]; const uint8_t *pk = T->p + q->off;
        for (int g = 0; g < T->ngood && q->hit < 0; g++) {
            const uint8_t *s3 = SET[T->good[g]];
            if (same_again(q->raw, q->rn, pk, q->used, s3[0], s3[1], s3[2])) q->hit = T->good[g];
        }
        if (q->hit < 0 && !T->stop_new)
            for (size_t k = 0; k < NSET && q->hit < 0; k++) {
                int seen = 0; for (int g = 0; g < T->ngood; g++) if (T->good[g] == (int)k) seen = 1;
                if (!seen && same_again(q->raw, q->rn, pk, q->used, SET[k][0], SET[k][1], SET[k][2])) q->hit = (int)k;
            }
    }
    return NULL;
}

static void run_trials(const uint8_t *p, cands_t *C, int threads) {
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    trial_t T; memset(&T, 0, sizeof T); T.p = p; T.c = C->c; pthread_mutex_init(&T.mu, NULL);
    int misses = 0; const size_t round = 64;   /* fixed: the same parts are opened on any number of cores */
    for (size_t a = 0; a < C->n; a += round) {
        if (T.stop_new && !T.ngood) break;
        T.a = a; T.b = a + round < C->n ? a + round : C->n; T.next = 0;
        int nt = (int)(T.b - T.a) < threads ? (int)(T.b - T.a) : threads;
        pthread_t th[64]; int made = 0;
        for (int k = 1; k < nt; k++) if (!pthread_create(&th[made], NULL, trial_worker, &T)) made++;
        trial_worker(&T);
        for (int k = 0; k < made; k++) pthread_join(th[k], NULL);
        for (size_t i = T.a; i < T.b; i++) {             /* share what worked, apply the give-up rule, in file order */
            int h = C->c[i].hit;
            if (h >= 0) { misses = 0; int seen = 0; for (int g = 0; g < T.ngood; g++) if (T.good[g] == h) seen = 1; if (!seen) T.good[T.ngood++] = h; }
            else if (++misses >= GIVE_UP) T.stop_new = 1;
        }
    }
    pthread_mutex_destroy(&T.mu);
}

/* ---------------------------------------------------------------- restore: every part packed again, on all cores */
typedef struct { const uint8_t *tab, *body; uint8_t *o; uint32_t np; const uint64_t *bpos; int next, bad; pthread_mutex_t mu; } rest_t;

static void *rest_worker(void *arg) {
    rest_t *R = arg;
    for (;;) {
        pthread_mutex_lock(&R->mu); int k = R->bad || R->next >= (int)R->np ? -1 : R->next++; pthread_mutex_unlock(&R->mu);
        if (k < 0) break;
        const uint8_t *t = R->tab + (size_t)k * REC;
        uint64_t off = get64(t); uint32_t plen = get32(t + 8), olen = get32(t + 12);
        z_stream z; memset(&z, 0, sizeof z);
        int ok = deflateInit2(&z, t[16], Z_DEFLATED, -15, t[17], t[18]) == Z_OK;
        if (ok) {
            z.next_in = (Bytef *)(R->body + R->bpos[k]); z.avail_in = olen; z.next_out = R->o + off; z.avail_out = plen;
            int r = deflate(&z, Z_FINISH);
            if (r == Z_OK && z.avail_out == 0) {          /* exactly full: one more call must say "finished" with no byte more */
                uint8_t spare[64]; z.next_out = spare; z.avail_out = sizeof spare; r = deflate(&z, Z_FINISH);
                if (z.avail_out != sizeof spare) r = Z_DATA_ERROR;
            }
            ok = r == Z_STREAM_END && z.total_out == plen;
            deflateEnd(&z);
        }
        if (!ok) { pthread_mutex_lock(&R->mu); R->bad = 1; pthread_mutex_unlock(&R->mu); }
    }
    return NULL;
}

/* ---------------------------------------------------------------- PNG (added 2026-11-09)
 * A PNG's picture data is ONE zlib stream cut into IDAT chunks. It is unpacked, and kept open only when packing it again
 * with zlib 1.3.1, at one of its standard settings, gives back
 * exactly the same stream. Stored form, all numbers little-endian:
 *   "LPN1" | u64 original size | u32 rest length | u32 where the IDAT chunks were in the rest | u32 IDAT count |
 *   u32 open length | u8 0 (zlib 1.3.1) | u8 level | u8 memLevel | u8 strategy | u8 window bits | 3 x 0 |
 *   count x u32 chunk data lengths | the rest (the PNG without its IDAT chunks) | the open picture data.
 * The restore packs the stream again (ONE writer, one thread), cuts it at the same lengths and writes each chunk with its
 * CRC-32 (ISO/IEC 15948). */
#define PHDR 36
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void pbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int png_restore(const uint8_t *p, size_t n, uint8_t **out, size_t *on) {
    if (n < PHDR || memcmp(p, "LPN1", 4)) return 1;
    uint64_t osz = get64(p + 4); uint32_t rlen = get32(p + 12), at = get32(p + 16), nid = get32(p + 20), olen = get32(p + 24);
    int pk = p[28], lv = p[29], mem = p[30], st = p[31], wb = p[32];
    if (pk != 0 || lv > 9 || mem < 1 || mem > 9 || st > 4 || wb < 8 || wb > 15 || nid == 0 || nid > (1u << 24)) return 1;
    if ((uint64_t)PHDR + (uint64_t)nid * 4 + rlen + olen != n || at > rlen || osz > ((uint64_t)1 << 40)) return 1;
    const uint8_t *lens = p + PHDR, *rest = lens + (size_t)nid * 4, *raw = rest + rlen;
    uint64_t zsum = 0; for (uint32_t k = 0; k < nid; k++) zsum += get32(lens + 4 * k);
    if (osz != (uint64_t)rlen + zsum + (uint64_t)nid * 12 || zsum > 0xFFFFFFFFu) return 1;
    uint8_t *z = malloc(zsum ? (size_t)zsum : 1), *o = malloc(osz ? (size_t)osz : 1);
    if (!z || !o) { free(z); free(o); return -1; }
    int bad;
    {
        z_stream s; memset(&s, 0, sizeof s); bad = 1;
        if (deflateInit2(&s, lv, Z_DEFLATED, wb, mem, st) == Z_OK) {
            s.next_in = (Bytef *)raw; s.avail_in = olen; s.next_out = z; s.avail_out = (uInt)zsum;
            int r = deflate(&s, Z_FINISH);
            if (r == Z_OK && s.avail_out == 0) { uint8_t sp[64]; s.next_out = sp; s.avail_out = sizeof sp; r = deflate(&s, Z_FINISH); if (s.avail_out != sizeof sp) r = Z_DATA_ERROR; }
            bad = !(r == Z_STREAM_END && s.total_out == zsum);
            deflateEnd(&s);
        }
    }
    if (bad) { free(z); free(o); return 1; }
    size_t w = 0, zp = 0;
    memcpy(o, rest, at); w = at;
    for (uint32_t k = 0; k < nid; k++) {
        uint32_t L = get32(lens + 4 * k);
        pbe32(o + w, L); memcpy(o + w + 4, "IDAT", 4); memcpy(o + w + 8, z + zp, L);
        uLong c = crc32(0L, o + w + 4, L + 4); pbe32(o + w + 8 + L, (uint32_t)c);
        w += 12 + L; zp += L;
    }
    memcpy(o + w, rest + at, rlen - at); w += rlen - at;
    free(z);
    *out = o; *on = (size_t)osz; return 0;
}

static int png_pack(const uint8_t *p, size_t n, uint8_t **out, size_t *on) {
    if (n < 8 + 12 || n > MAX_TOTAL || memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return 1;
    /* the chunks: every IDAT next to each other (else the PNG stays as it is) */
    size_t pos = 8, first = 0, endi = 0; uint32_t nid = 0; uint64_t zsum = 0; int seen_end = 0;
    while (pos + 12 <= n && !seen_end) {
        uint32_t L = be32(p + pos);
        if (L > n - pos - 12) return 1;
        int idat = !memcmp(p + pos + 4, "IDAT", 4);
        if (idat) { if (nid && endi != pos) return 1; if (!nid) first = pos; nid++; zsum += L; endi = pos + 12 + L; }
        if (!memcmp(p + pos + 4, "IEND", 4)) seen_end = 1;
        pos += 12 + (size_t)L;
    }
    if (!nid || zsum < MIN_PACKED || zsum > 0xFFFFFFFFu) return 1;
    uint8_t *z = malloc((size_t)zsum), *lens = malloc((size_t)nid * 4);
    if (!z || !lens) { free(z); free(lens); return -1; }
    size_t zp = 0; uint32_t k = 0;
    for (size_t q = first; q < endi; ) { uint32_t L = be32(p + q); put32(lens + 4 * k++, L); memcpy(z + zp, p + q + 8, L); zp += L; q += 12 + L; }
    int rc = 1; uint8_t *raw = NULL; size_t rn = 0, used = 0;
    uint8_t cmf = z[0], flg = z[1];
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 || (flg & 32)) goto done;
    int wb = (cmf >> 4) + 8;
    { int r = inflate_at(z + 2, (size_t)zsum - 2, &raw, &rn, &used); if (r < 0) { rc = -1; goto done; } if (r) goto done; }
    if (used + 2 + 4 != zsum || rn > MAX_OPEN * 8) goto done;     /* nothing after the stream but its 4-byte check */
    int pkr = -1, set = -1;
    for (size_t s3 = 0; s3 < NSET && pkr < 0; s3++)              /* the PNG library's default (level 6, filtered) comes early */
        for (int which = 0; which < 1 && pkr < 0; which++) {
            static const int order[NSET] = {18, 0, 19, 1, 20, 2, 9, 27, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17,
                                            21, 22, 23, 24, 25, 26, 28, 29, 30, 31, 32, 33, 34, 35};
            const uint8_t *t = SET[order[s3]];
            int ok = same_again_w(raw, rn, z, (size_t)zsum, t[0], t[1], t[2], wb);
            if (ok) { pkr = which; set = order[s3]; }
        }
    if (pkr < 0) goto done;
    size_t rlen = n - (endi - first), sz = PHDR + (size_t)nid * 4 + rlen + rn;
    uint8_t *o = malloc(sz);
    if (!o) { rc = -1; goto done; }
    memcpy(o, "LPN1", 4); put64(o + 4, n); put32(o + 12, (uint32_t)rlen); put32(o + 16, (uint32_t)first); put32(o + 20, nid);
    put32(o + 24, (uint32_t)rn); o[28] = (uint8_t)pkr; o[29] = SET[set][0]; o[30] = SET[set][1]; o[31] = SET[set][2]; o[32] = (uint8_t)wb;
    o[33] = o[34] = o[35] = 0;
    memcpy(o + PHDR, lens, (size_t)nid * 4);
    uint8_t *b = o + PHDR + (size_t)nid * 4;
    memcpy(b, p, first); memcpy(b + first, p + endi, n - endi); memcpy(b + rlen, raw, rn);
    uint8_t *back = NULL; size_t bn = 0;                          /* the check: the stored form gives back the PNG, byte for byte */
    int r = png_restore(o, sz, &back, &bn);
    if (r < 0) { free(o); rc = -1; goto done; }
    if (r || bn != n || memcmp(back, p, n)) { free(back); free(o); goto done; }
    free(back);
    *out = o; *on = sz; rc = 0;
done:
    free(z); free(lens); free(raw);
    return rc;
}

/* ---------------------------------------------------------------- JPEG (added 2026-11-09, steps J2 / J3)
 * A baseline JPEG is read down to its DCT coefficients (ljpeg.c, from the JPEG standard alone) and checked to be written
 * back as the same bytes; the coefficients are then coded (lcoef.c). Stored form, all numbers little-endian:
 *   "LJP3" | u64 original size | u32 header length | u32 tail length | u32 blocks | u8 padding bit | u8 blocks per MCU |
 *   10 x u8 colour part of each block of an MCU | u32 MCUs per row | 10 x u8 each block's column (high 4 bits) and row
 *   (low 4 bits) inside its MCU | 2 x u8 0 | the header bytes (up to the scan) | the tail (after the scan) | the coded
 *   coefficients. The restore decodes them and writes the scan with the file's own tables (ONE writer). */
#define JHDR 52
static int jpeg_restore(const uint8_t *p, size_t n, uint8_t **out, size_t *on) {
    if (n < JHDR || memcmp(p, "LJP3", 4)) return 1;
    uint64_t osz = get64(p + 4); uint32_t hl = get32(p + 12), tl = get32(p + 16), nb = get32(p + 20);
    int pad = p[24], pm = p[25]; int bc[10];
    if (pad > 1 || pm < 1 || pm > 10 || nb == 0 || nb > (1u << 26) || (uint64_t)JHDR + hl + tl > n || osz > ((uint64_t)1 << 34)) return 1;
    for (int k = 0; k < 10; k++) bc[k] = p[26 + k] & 3;
    lcoef_layout L; memset(&L, 0, sizeof L); L.per_mcu = pm; L.mcux = (int)get32(p + 36);
    if (get32(p + 36) == 0 || get32(p + 36) > (1u << 20)) return 1;
    for (int k = 0; k < pm; k++) { L.comp[k] = bc[k]; L.bx[k] = p[40 + k] >> 4; L.by[k] = p[40 + k] & 15; }
    const uint8_t *head = p + JHDR, *tail = head + hl, *cod = tail + tl; size_t cn = n - JHDR - hl - tl;
    int16_t *cf = malloc((size_t)nb * 64 * sizeof *cf);
    if (!cf) return -1;
    int r = lcoef_decode(cod, cn, (int)nb, &L, cf);
    if (r) { free(cf); return r < 0 ? -1 : 1; }
    ljpeg_info I; memset(&I, 0, sizeof I); I.head_len = hl; I.pad_bit = pad;
    I.nblocks = (int)nb; I.per_mcu = pm; I.mcux = L.mcux;
    for (int k = 0; k < pm; k++) { I.blk_comp[k] = L.comp[k]; I.blk_x[k] = L.bx[k]; I.blk_y[k] = L.by[k]; }
    uint8_t *sc = NULL; size_t sn = 0;
    r = ljpeg_write_scan(head, &I, cf, &sc, &sn);
    free(cf);
    if (r) { free(sc); return r == LJ_MEM ? -1 : 1; }
    if ((uint64_t)hl + sn + tl != osz) { free(sc); return 1; }
    uint8_t *o = malloc(osz ? (size_t)osz : 1);
    if (!o) { free(sc); return -1; }
    memcpy(o, head, hl); memcpy(o + hl, sc, sn); memcpy(o + hl + sn, tail, tl);
    free(sc);
    *out = o; *on = (size_t)osz; return 0;
}

static int jpeg_pack(const uint8_t *p, size_t n, uint8_t **out, size_t *on) {
    ljpeg_info I; int16_t *cf = NULL;
    int r = ljpeg_read(p, n, &I, &cf);
    if (r == LJ_MEM) return -1;
    if (r) return 1;
    uint8_t *cod = NULL; size_t cn = 0;
    lcoef_layout L; memset(&L, 0, sizeof L); L.per_mcu = I.per_mcu; L.mcux = I.mcux;
    for (int k = 0; k < I.per_mcu; k++) { if (I.blk_x[k] > 15 || I.blk_y[k] > 15) { free(cf); return 1; }
                                          L.comp[k] = I.blk_comp[k]; L.bx[k] = I.blk_x[k]; L.by[k] = I.blk_y[k]; }
    r = lcoef_encode(cf, I.nblocks, &L, &cod, &cn);
    free(cf);
    if (r) return r < 0 ? -1 : 1;
    size_t sz = JHDR + I.head_len + I.tail_len + cn;
    if (sz >= n) { free(cod); return 1; }                         /* never bigger */
    uint8_t *o = malloc(sz);
    if (!o) { free(cod); return -1; }
    memset(o, 0, JHDR); memcpy(o, "LJP3", 4); put64(o + 4, n); put32(o + 12, (uint32_t)I.head_len); put32(o + 16, (uint32_t)I.tail_len);
    put32(o + 20, (uint32_t)I.nblocks); o[24] = (uint8_t)I.pad_bit; o[25] = (uint8_t)I.per_mcu;
    for (int k = 0; k < I.per_mcu; k++) { o[26 + k] = (uint8_t)I.blk_comp[k]; o[40 + k] = (uint8_t)(L.bx[k] << 4 | L.by[k]); }
    put32(o + 36, (uint32_t)I.mcux);
    memcpy(o + JHDR, p, I.head_len); memcpy(o + JHDR + I.head_len, p + I.scan_off + I.scan_len, I.tail_len);
    memcpy(o + JHDR + I.head_len + I.tail_len, cod, cn);
    free(cod);
    uint8_t *back = NULL; size_t bn = 0;                          /* the check: the stored form gives back the JPEG, byte for byte */
    r = jpeg_restore(o, sz, &back, &bn);
    if (r < 0) { free(o); return -1; }
    if (r || bn != n || memcmp(back, p, n)) { free(back); free(o); return 1; }
    free(back);
    *out = o; *on = sz; return 0;
}

int lopen_restore(const uint8_t *p, size_t n, int threads, uint8_t **out, size_t *on) {
    if (n >= 4 && !memcmp(p, "LJP3", 4)) return jpeg_restore(p, n, out, on);
    if (n >= 4 && !memcmp(p, "LPN1", 4)) return png_restore(p, n, out, on);
    if (n < HDR || memcmp(p, MAGIC, 4)) return 1;
    uint32_t np = get32(p + 4); uint64_t osz = get64(p + 8);
    if (np == 0 || (uint64_t)np * REC > n - HDR || osz > ((uint64_t)1 << 40)) return 1;
    const uint8_t *tab = p + HDR, *body = tab + (size_t)np * REC; uint64_t blen = (uint64_t)(p + n - body);
    uint64_t *bpos = malloc(sizeof *bpos * np);
    uint8_t *o = malloc(osz ? (size_t)osz : 1);
    if (!bpos || !o) { free(bpos); free(o); return -1; }
    /* the table is checked first, and the plain bytes between parts are copied */
    uint64_t opos = 0, b = 0; int rc = 1;
    for (uint32_t k = 0; k < np; k++) {
        const uint8_t *t = tab + (size_t)k * REC;
        uint64_t off = get64(t); uint32_t plen = get32(t + 8), olen = get32(t + 12);
        if (off < opos || off - opos > blen - b || plen > osz - off || t[16] > 9 || t[17] < 1 || t[17] > 9 || t[18] > 4) goto out;
        memcpy(o + opos, body + b, (size_t)(off - opos)); b += off - opos;
        if (olen > blen - b) goto out;
        bpos[k] = b; b += olen; opos = off + plen;
    }
    if (blen - b != osz - opos) goto out;
    memcpy(o + opos, body + b, (size_t)(osz - opos));
    rest_t R; memset(&R, 0, sizeof R); R.tab = tab; R.body = body; R.o = o; R.np = np; R.bpos = bpos; pthread_mutex_init(&R.mu, NULL);
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    if ((uint32_t)threads > np) threads = (int)np;
    pthread_t th[64]; int made = 0;
    for (int k = 1; k < threads; k++) if (!pthread_create(&th[made], NULL, rest_worker, &R)) made++;
    rest_worker(&R);
    for (int k = 0; k < made; k++) pthread_join(th[k], NULL);
    pthread_mutex_destroy(&R.mu);
    if (R.bad) goto out;
    free(bpos); *out = o; *on = (size_t)osz; return 0;
out:
    free(bpos); free(o); return rc;
}

int lopen_pack(const uint8_t *p, size_t n, int threads, uint8_t **out, size_t *on) {
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return jpeg_pack(p, n, out, on);
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return png_pack(p, n, out, on);
    cands_t C; memset(&C, 0, sizeof C);
    int oom = 0, rc = 1;
    if (n >= 5 && !memcmp(p, "%PDF-", 5)) scan_pdf(&C, p, n, &oom);
    else if (n >= 4 && !memcmp(p, "PK\3\4", 4)) scan_zip(&C, p, n, &oom);
    if (oom) { rc = -1; goto done; }
    if (!C.n) goto done;
    run_trials(p, &C, threads);
    size_t np = 0, popen = 0, ppack = 0;
    for (size_t i = 0; i < C.n; i++) if (C.c[i].hit >= 0) { np++; popen += C.c[i].rn; ppack += C.c[i].used; }
    if (!np || np > 0xFFFFFFFFu) goto done;
    size_t sz = HDR + np * REC + (n - ppack) + popen;
    uint8_t *o = malloc(sz);
    if (!o) { rc = -1; goto done; }
    memcpy(o, MAGIC, 4); put32(o + 4, (uint32_t)np); put64(o + 8, n);
    uint8_t *b = o + HDR + np * REC; uint64_t pos = 0; size_t k = 0;
    for (size_t i = 0; i < C.n; i++) {
        const cand_t *q = &C.c[i]; if (q->hit < 0) continue;
        uint8_t *t = o + HDR + k++ * REC; const uint8_t *s3 = SET[q->hit];
        put64(t, q->off); put32(t + 8, q->used); put32(t + 12, (uint32_t)q->rn); t[16] = s3[0]; t[17] = s3[1]; t[18] = s3[2]; t[19] = 0;
        memcpy(b, p + pos, (size_t)(q->off - pos)); b += q->off - pos;
        memcpy(b, q->raw, q->rn); b += q->rn; pos = q->off + q->used;
    }
    memcpy(b, p + pos, n - pos);
    /* the check: the stored form must give back the file, byte for byte */
    uint8_t *back = NULL; size_t bn = 0;
    int r = lopen_restore(o, sz, threads, &back, &bn);
    if (r < 0) { free(o); rc = -1; goto done; }
    if (r || bn != n || memcmp(back, p, n)) { free(back); free(o); goto done; }
    free(back);
    *out = o; *on = sz; rc = 0;
done:
    for (size_t i = 0; i < C.n; i++) free(C.c[i].raw);
    free(C.c);
    return rc;
}
