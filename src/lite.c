/* Lite archive - see lite.h for the layout.
 * Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
#include "lite.h"
#include "xola.h"
#include "lopen.h"
#include <stdlib.h>
#include <string.h>

static const char HMAGIC[8] = {'L','I','T','E','A','R','C','1'};
static const char TMAGIC[8] = {'L','I','T','E','E','N','D','1'};
#define TRAILER 40
#define MAX_INDEX (256u << 20)      /* an index larger than this is refused */
#define MAX_ENTRIES 4000000u

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

void lite_default_opts(lite_opts *o) { o->level = LITE_LEVEL; o->threads = 4; o->piece_mb = 32; o->lz_block_mb = 4; }

const char *lite_strerror(int e) {
    switch (e) {
    case LITE_OK: return "ok";
    case LITE_E_MEM: return "out of memory";
    case LITE_E_FORMAT: return "not a Lite archive, or its index is damaged";
    case LITE_E_DAMAGED: return "damaged";
    case LITE_E_ARG: return "bad argument";
    case LITE_E_IO: return "read or write failed";
    case LITE_E_STOPPED: return "stopped";
    }
    return "error";
}

int lite_name_safe(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 4096 || s[0] == '/') return 0;
    const char *part = s;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' || c == 0) {
            size_t len = (size_t)(s + i - part);
            if (len == 0) return 0;
            if (len == 1 && part[0] == '.') return 0;
            if (len == 2 && part[0] == '.' && part[1] == '.') return 0;
            part = s + i + 1;
            continue;
        }
        if (c < 32 || c == 127 || c == '\\' || c == ':') return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ a growing byte buffer */
typedef struct { uint8_t *p; size_t n, cap; } buf_t;
static int bgrow(buf_t *b, size_t add) {
    if (b->n + add <= b->cap) return 0;
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + add) c *= 2;
    uint8_t *q = realloc(b->p, c);
    if (!q) return -1;
    b->p = q; b->cap = c; return 0;
}
static int bput(buf_t *b, const void *p, size_t n) { if (bgrow(b, n)) return -1; memcpy(b->p + b->n, p, n); b->n += n; return 0; }

/* ------------------------------------------------------------------ packing */
typedef struct { uint32_t name_off; uint16_t name_len; uint8_t is_dir, opened; uint64_t size, offset; int64_t mtime; uint32_t crc; uint64_t osize; uint32_t ocrc; } ent_t;

struct lite_packer {
    lite_write_fn w; void *wctx;
    xola_params xp;
    uint64_t piece_size, stream_len, pos;   /* pos = bytes written to the archive */
    uint8_t *piece; size_t fill;
    buf_t names, pieces;                    /* pieces: u64 offset, u64 length each */
    ent_t *ents; size_t nents, capents;
    int has_opened;
    int open_file;                          /* the last entry is a file still taking data */
    int err;
    volatile int stop;
    lite_stats st;
};

volatile int *lite_pack_stop_flag(lite_packer *pk) { return &pk->stop; }

static int wr(lite_packer *pk, const uint8_t *p, size_t n) {
    if (pk->w(pk->wctx, p, n)) return LITE_E_IO;
    pk->pos += n; pk->st.out_bytes += n; return LITE_OK;
}

/* one piece: packed with way A (the LZ coder) at Lite's one setting - one method for every piece and block of the archive */
static int flush_piece(lite_packer *pk) {
    if (!pk->fill) return LITE_OK;
    uint8_t *arc = NULL; size_t an = 0;
    int rc = xola_pack(pk->piece, pk->fill, &pk->xp, &arc, &an);
    if (rc) { free(arc); return rc == XOLA_E_MEM ? LITE_E_MEM : LITE_E_ARG; }
    uint8_t rec[16]; put64(rec, pk->pos); put64(rec + 8, an);
    rc = bput(&pk->pieces, rec, 16) ? LITE_E_MEM : wr(pk, arc, an);
    free(arc);
    if (rc) return rc;
    pk->fill = 0; pk->st.pieces++;
    return LITE_OK;
}

lite_packer *lite_pack_begin(lite_write_fn w, void *wctx, const lite_opts *o, int *err) {
    lite_opts d; lite_default_opts(&d); if (!o) o = &d;
    int e = LITE_OK;
    lite_packer *pk = NULL;
    if (!w || o->piece_mb < 1 || o->piece_mb > 256 || o->lz_block_mb < 1 || o->lz_block_mb > 16) { e = LITE_E_ARG; goto out; }
    pk = calloc(1, sizeof *pk);
    if (!pk) { e = LITE_E_MEM; goto out; }
    pk->w = w; pk->wctx = wctx;
    xola_default_params(&pk->xp);
    pk->xp.level = LITE_LEVEL; pk->xp.threads = o->threads < 1 ? 1 : o->threads;
    pk->xp.block_kb = o->lz_block_mb * 1024;
    pk->st.method = LITE_LZ;
    pk->piece_size = (uint64_t)o->piece_mb << 20;
    pk->piece = malloc((size_t)pk->piece_size);
    if (!pk->piece) { e = LITE_E_MEM; goto out; }
    uint8_t h[16]; memcpy(h, HMAGIC, 8); put32(h + 8, 2); put32(h + 12, 0);   /* the way is in the index (known after piece 1) */
    e = wr(pk, h, 16);
out:
    if (e && pk) { lite_pack_free(pk); pk = NULL; }
    if (err) *err = e;
    return pk;
}

static void close_file(lite_packer *pk) {
    if (pk->open_file) { ent_t *t = &pk->ents[pk->nents - 1]; t->size = pk->stream_len - t->offset; pk->open_file = 0; }
}

int lite_pack_entry(lite_packer *pk, const char *name, int64_t mtime, int is_dir) {
    if (pk->err) return pk->err;
    if (!name || !lite_name_safe(name) || pk->nents >= MAX_ENTRIES) return LITE_E_ARG;
    close_file(pk);
    if (pk->nents == pk->capents) {
        size_t c = pk->capents ? pk->capents * 2 : 256;
        ent_t *q = realloc(pk->ents, c * sizeof *q);
        if (!q) return pk->err = LITE_E_MEM;
        pk->ents = q; pk->capents = c;
    }
    size_t len = strlen(name);
    ent_t *t = &pk->ents[pk->nents];
    memset(t, 0, sizeof *t);
    t->name_off = (uint32_t)pk->names.n; t->name_len = (uint16_t)len; t->is_dir = is_dir ? 1 : 0;
    t->offset = pk->stream_len; t->mtime = mtime;
    if (pk->names.n + len > 0xFFFFFFFFu || bput(&pk->names, name, len)) return pk->err = LITE_E_MEM;
    pk->nents++;
    if (is_dir) pk->st.folders++; else { pk->st.files++; pk->open_file = 1; }
    return LITE_OK;
}

uint32_t lite_pack_count(const lite_packer *pk) { return (uint32_t)pk->nents; }

int lite_pack_copy(lite_packer *pk, const char *name, int64_t mtime, uint32_t same_as) {
    if (pk->err) return pk->err;
    if (same_as >= pk->nents || pk->ents[same_as].is_dir) return LITE_E_ARG;
    close_file(pk);
    ent_t src = pk->ents[same_as];
    int rc = lite_pack_entry(pk, name, mtime, 0);
    if (rc) return rc;
    ent_t *t = &pk->ents[pk->nents - 1];
    t->offset = src.offset; t->size = src.size; t->crc = src.crc; t->opened = src.opened; t->osize = src.osize; t->ocrc = src.ocrc;
    pk->open_file = 0;                 /* takes no data of its own */
    pk->st.in_bytes += src.opened ? src.osize : src.size;
    return LITE_OK;
}

int lite_pack_opened(lite_packer *pk, const char *name, int64_t mtime, uint64_t osize, uint32_t ocrc, const uint8_t *o, size_t on) {
    int rc = lite_pack_entry(pk, name, mtime, 0);
    if (rc) return rc;
    rc = lite_pack_data(pk, o, on);
    if (rc) return rc;
    ent_t *t = &pk->ents[pk->nents - 1];
    t->opened = 1; t->osize = osize; t->ocrc = ocrc;
    pk->st.in_bytes += osize - on;     /* the file counts with its own size */
    pk->has_opened = 1; pk->st.opened++;
    return LITE_OK;
}

/* a PNG opened is kept only when its open form packs smaller than the PNG itself (both packed alone, one thread, at
 * Lite's setting); otherwise the PNG is stored as it is - Lite never makes a PNG bigger by opening it */
static int open_pays(const uint8_t *p, size_t n, const uint8_t *o, size_t on) {
    if (on < 4 || memcmp(o, "LPN1", 4)) return 1;
    xola_params xp; xola_default_params(&xp); xp.level = LITE_LEVEL; xp.threads = 1;
    uint8_t *a = NULL, *b = NULL; size_t an = 0, bn = 0;
    int ra = xola_pack(o, on, &xp, &a, &an), rb = ra ? ra : xola_pack(p, n, &xp, &b, &bn);
    free(a); free(b);
    if (ra || rb) return -1;
    return an < bn;
}

int lite_pack_file(lite_packer *pk, const char *name, int64_t mtime, const uint8_t *p, size_t n, int allow_open) {
    uint8_t *o = NULL; size_t on = 0;
    int r = allow_open && lopen_wanted(p, n) ? lopen_pack(p, n, (int)pk->xp.threads, &o, &on) : 1;
    if (!r) { int w = open_pays(p, n, o, on); if (w < 0) r = -1; else if (!w) { free(o); o = NULL; r = 1; } }
    if (r < 0) return pk->err = LITE_E_MEM;
    if (r) { int rc = lite_pack_entry(pk, name, mtime, 0); return rc ? rc : lite_pack_data(pk, p, n); }
    int rc = lite_pack_opened(pk, name, mtime, n, xola_crc32c(0, p, n), o, on);
    free(o);
    return rc;
}

/* ---- several files opened / restored at once: each file is one job with its own single writer; the jobs share the
 * cores (a job that is a document still uses its share of cores for its own trials). The results do not depend on how
 * many cores ran them. */
#include <pthread.h>
typedef struct { void *jobs; int count, next, inner; void (*run)(void *job, int inner); size_t size; pthread_mutex_t mu; } pool_t;
static void *pool_worker(void *arg) {
    pool_t *P = arg;
    for (;;) {
        pthread_mutex_lock(&P->mu); int k = P->next < P->count ? P->next++ : -1; pthread_mutex_unlock(&P->mu);
        if (k < 0) return NULL;
        P->run((uint8_t *)P->jobs + (size_t)k * P->size, P->inner);
    }
}
static void pool_run(void *jobs, size_t size, int count, int threads, void (*run)(void *, int)) {
    if (count <= 0) return;
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    int w = count < threads ? count : threads;
    pool_t P; memset(&P, 0, sizeof P); P.jobs = jobs; P.count = count; P.run = run; P.size = size;
    P.inner = threads / w < 1 ? 1 : threads / w;
    pthread_mutex_init(&P.mu, NULL);
    pthread_t th[64]; int made = 0;
    for (int k = 1; k < w; k++) if (!pthread_create(&th[made], NULL, pool_worker, &P)) made++;
    pool_worker(&P);
    for (int k = 0; k < made; k++) pthread_join(th[k], NULL);
    pthread_mutex_destroy(&P.mu);
}
static void open_job(void *v, int inner) {
    lite_open_job *J = v;
    J->out = NULL; J->on = 0;
    J->crc = xola_crc32c(0, J->p, J->n);
    J->rc = lopen_wanted(J->p, J->n) ? lopen_pack(J->p, J->n, inner, &J->out, &J->on) : 1;
    if (!J->rc) { int w = open_pays(J->p, J->n, J->out, J->on); if (w < 0) J->rc = -1; else if (!w) { free(J->out); J->out = NULL; J->on = 0; J->rc = 1; } }
}
void lite_open_many(lite_open_job *jobs, int count, int threads) {
    pool_run(jobs, sizeof *jobs, count, threads, open_job);
    for (int k = 0; k < count; k++)            /* short of memory with the others running: tried again on its own */
        if (jobs[k].rc < 0) { free(jobs[k].out); jobs[k].out = NULL; open_job(&jobs[k], threads); }
}
static void restore_job(void *v, int inner) {
    lite_restore_job *J = v;
    J->out = NULL; J->on = 0;
    int r = lopen_restore(J->stored, J->sn, inner, &J->out, &J->on);
    if (r < 0) { J->rc = LITE_E_MEM; return; }
    if (r || J->on != J->size || xola_crc32c(0, J->out, J->on) != J->crc) { free(J->out); J->out = NULL; J->on = 0; J->rc = LITE_E_DAMAGED; return; }
    J->rc = LITE_OK;
}
void lite_restore_many(lite_restore_job *jobs, int count, int threads) {
    pool_run(jobs, sizeof *jobs, count, threads, restore_job);
    for (int k = 0; k < count; k++) if (jobs[k].rc == LITE_E_MEM) restore_job(&jobs[k], threads);
}

int lite_pack_data(lite_packer *pk, const uint8_t *p, size_t n) {
    if (pk->err) return pk->err;
    if (pk->stop) return pk->err = LITE_E_STOPPED;
    if (!pk->open_file) return LITE_E_ARG;
    ent_t *t = &pk->ents[pk->nents - 1];
    t->crc = xola_crc32c(t->crc, p, n);
    pk->stream_len += n; pk->st.in_bytes += n;
    while (n) {
        size_t k = (size_t)(pk->piece_size - pk->fill); if (k > n) k = n;
        memcpy(pk->piece + pk->fill, p, k); pk->fill += k; p += k; n -= k;
        if (pk->fill == pk->piece_size) { int rc = flush_piece(pk); if (rc) return pk->err = rc; }
        if (pk->stop) return pk->err = LITE_E_STOPPED;
    }
    return LITE_OK;
}

int lite_pack_end(lite_packer *pk, lite_stats *st) {
    if (pk->err) return pk->err;
    close_file(pk);
    int rc = flush_piece(pk);
    if (rc) return pk->err = rc;
    buf_t ix = {0};
    uint8_t h[36];
    uint32_t np = (uint32_t)(pk->pieces.n / 16);
    memcpy(h, "LDIR", 4); put32(h + 4, pk->has_opened ? 3 : 2);   /* 3 only when a file is stored opened */ put64(h + 8, pk->piece_size); put64(h + 16, pk->stream_len);
    put32(h + 24, np); put32(h + 28, (uint32_t)pk->nents); put32(h + 32, (uint32_t)LITE_LZ);
    rc = bput(&ix, h, 36) || bput(&ix, pk->pieces.p, pk->pieces.n) ? LITE_E_MEM : LITE_OK;
    for (size_t i = 0; !rc && i < pk->nents; i++) {
        const ent_t *t = &pk->ents[i];
        uint8_t e[44]; e[0] = t->is_dir ? 1 : t->opened ? 2 : 0; e[1] = 0; e[2] = (uint8_t)t->name_len; e[3] = (uint8_t)(t->name_len >> 8);
        put64(e + 4, t->size); put64(e + 12, t->offset); put64(e + 20, (uint64_t)t->mtime); put32(e + 28, t->crc);
        size_t el = 32;
        if (t->opened) { put64(e + 32, t->osize); put32(e + 40, t->ocrc); el = 44; }   /* kind 2: the original's size and CRC */
        if (bput(&ix, e, el) || bput(&ix, pk->names.p + t->name_off, t->name_len)) rc = LITE_E_MEM;
    }
    uint8_t *arc = NULL; size_t an = 0;
    if (!rc) { xola_params xp = pk->xp; xp.level = LITE_LEVEL; rc = xola_pack(ix.p, ix.n, &xp, &arc, &an) ? LITE_E_MEM : LITE_OK; }
    free(ix.p);
    uint64_t ioff = pk->pos;
    if (!rc) rc = wr(pk, arc, an);
    free(arc);
    if (!rc) {
        uint8_t tr[TRAILER];
        put64(tr, ioff); put64(tr + 8, an); put64(tr + 16, pk->stream_len);
        put32(tr + 24, xola_crc32c(0, tr, 24)); put32(tr + 28, 0); memcpy(tr + 32, TMAGIC, 8);
        rc = wr(pk, tr, TRAILER);
    }
    if (rc) return pk->err = rc;
    if (st) *st = pk->st;
    pk->err = LITE_E_ARG;   /* finished: no more entries */
    return LITE_OK;
}

void lite_pack_free(lite_packer *pk) {
    if (!pk) return;
    free(pk->piece); free(pk->names.p); free(pk->pieces.p); free(pk->ents); free(pk);
}

/* ------------------------------------------------------------------ reading */
typedef struct { uint64_t off, len; } piece_t;

struct lite_reader {
    lite_pread_fn rd; void *rctx;
    int threads;
    uint64_t piece_size, stream_len;
    uint32_t np, ne;
    int method;
    piece_t *pc;
    lite_entry *ents;
    char *names;               /* names copied with a 0 after each */
    /* one decoded piece kept */
    int64_t cur;               /* piece number in the cache, -1 none */
    uint8_t *data; size_t dlen;
    int cur_all_bad;           /* the whole piece is not trusted */
    uint32_t nbad; uint64_t badblk[16]; uint64_t blk;   /* damaged blocks of the cached piece */
};

static int rd_all(lite_reader *r, uint8_t *p, size_t n, uint64_t off) {
    while (n) {
        int64_t k = r->rd(r->rctx, p, n, off);
        if (k <= 0) return -1;
        p += k; n -= (size_t)k; off += (uint64_t)k;
    }
    return 0;
}

lite_reader *lite_open(lite_pread_fn rd, void *rctx, uint64_t asz, int threads, int *err) {
    int e = LITE_E_FORMAT;
    uint8_t *iarc = NULL, *ix = NULL; size_t ixn = 0;
    lite_reader *r = calloc(1, sizeof *r);
    if (!r) { e = LITE_E_MEM; goto fail; }
    r->rd = rd; r->rctx = rctx; r->threads = threads < 1 ? 1 : threads; r->cur = -1;
    if (asz < 16 + TRAILER) goto fail;
    uint8_t h[16], tr[TRAILER];
    if (rd_all(r, h, 16, 0) || rd_all(r, tr, TRAILER, asz - TRAILER)) { e = LITE_E_IO; goto fail; }
    if (memcmp(h, HMAGIC, 8) || get32(h + 8) < 1 || get32(h + 8) > 2 || memcmp(tr + 32, TMAGIC, 8) || get32(tr + 24) != xola_crc32c(0, tr, 24))
        goto fail;
    uint64_t ioff = get64(tr), ilen = get64(tr + 8);
    r->stream_len = get64(tr + 16);
    if (ioff < 16 || ilen == 0 || ilen > MAX_INDEX || ioff + ilen != asz - TRAILER) goto fail;
    iarc = malloc((size_t)ilen);
    if (!iarc) { e = LITE_E_MEM; goto fail; }
    if (rd_all(r, iarc, (size_t)ilen, ioff)) { e = LITE_E_IO; goto fail; }
    xola_report rep;
    if (xola_unpack(iarc, (size_t)ilen, 1, &ix, &ixn, &rep) != XOLA_OK) goto fail;
    if (ixn < 32 || memcmp(ix, "LDIR", 4) || get32(ix + 4) < 1 || get32(ix + 4) > 3) goto fail;
    uint32_t ver = get32(ix + 4);
    size_t p = ver >= 2 ? 36 : 32;
    if (ixn < p) goto fail;
    r->method = p == 36 ? (int)get32(ix + 32) : LITE_LZ;
    if (r->method != LITE_LZ) goto fail;
    r->piece_size = get64(ix + 8);
    r->np = get32(ix + 24); r->ne = get32(ix + 28);
    if (get64(ix + 16) != r->stream_len || r->piece_size == 0 || r->piece_size > (256u << 20) || r->ne > MAX_ENTRIES) goto fail;
    if ((r->stream_len + r->piece_size - 1) / r->piece_size != r->np) goto fail;
    if ((uint64_t)r->np * 16 > ixn - p) goto fail;
    r->pc = calloc(r->np ? r->np : 1, sizeof *r->pc);
    r->ents = calloc(r->ne ? r->ne : 1, sizeof *r->ents);
    r->names = malloc(ixn + r->ne + 1);
    if (!r->pc || !r->ents || !r->names) { e = LITE_E_MEM; goto fail; }
    uint64_t prev = 16;
    for (uint32_t i = 0; i < r->np; i++, p += 16) {
        r->pc[i].off = get64(ix + p); r->pc[i].len = get64(ix + p + 8);
        if (r->pc[i].off != prev || r->pc[i].len == 0 || r->pc[i].len > ioff - r->pc[i].off) goto fail;
        prev = r->pc[i].off + r->pc[i].len;
    }
    if (prev != ioff) goto fail;
    size_t no = 0;
    for (uint32_t i = 0; i < r->ne; i++) {
        if (ixn - p < 32) goto fail;
        const uint8_t *q = ix + p;
        uint32_t nl = q[2] | (uint32_t)q[3] << 8;
        size_t el = q[0] == 2 ? 44 : 32;
        if (q[0] > (ver >= 3 ? 2 : 1) || ixn - p < el || ixn - p - el < nl) goto fail;
        lite_entry *t = &r->ents[i];
        t->is_dir = q[0] == 1; t->opened = q[0] == 2;
        t->stored = get64(q + 4); t->offset = get64(q + 12); t->mtime = (int64_t)get64(q + 20); t->stored_crc = get32(q + 28);
        if (t->opened) { t->size = get64(q + 32); t->crc = get32(q + 40); } else { t->size = t->stored; t->crc = t->stored_crc; }
        memcpy(r->names + no, q + el, nl); r->names[no + nl] = 0;
        t->name = r->names + no; no += nl + 1; p += el + nl;
        if (t->offset > r->stream_len || t->stored > r->stream_len - t->offset || (t->is_dir && t->stored)) goto fail;
        if (!lite_name_safe(t->name)) goto fail;     /* never a path that leaves the folder */
    }
    if (p != ixn) goto fail;
    free(iarc); free(ix);
    if (err) *err = LITE_OK;
    return r;
fail:
    free(iarc); free(ix);
    lite_close(r);
    if (err) *err = e;
    return NULL;
}

uint32_t lite_count(const lite_reader *r) { return r->ne; }
const lite_entry *lite_get(const lite_reader *r, uint32_t i) { return i < r->ne ? &r->ents[i] : NULL; }
uint64_t lite_stream_size(const lite_reader *r) { return r->stream_len; }
int lite_method(const lite_reader *r) { return r->method; }

static int load_piece(lite_reader *r, uint32_t k) {
    if (r->cur == (int64_t)k) return LITE_OK;
    free(r->data); r->data = NULL; r->dlen = 0; r->cur = -1;
    uint8_t *arc = malloc((size_t)r->pc[k].len);
    if (!arc) return LITE_E_MEM;
    if (rd_all(r, arc, (size_t)r->pc[k].len, r->pc[k].off)) { free(arc); return LITE_E_IO; }
    xola_report rep; memset(&rep, 0, sizeof rep);
    int rc = xola_unpack(arc, (size_t)r->pc[k].len, r->threads, &r->data, &r->dlen, &rep);
    free(arc);
    uint64_t want = k + 1 < r->np ? r->piece_size : r->stream_len - (uint64_t)k * r->piece_size;
    r->cur_all_bad = 0; r->nbad = 0; r->blk = rep.block_size;
    if (rc == XOLA_E_DAMAGED && r->data && r->dlen == want) {
        if (rep.ndamaged > 16 || !rep.block_size) r->cur_all_bad = 1;
        else { r->nbad = rep.ndamaged; for (uint32_t i = 0; i < rep.ndamaged; i++) r->badblk[i] = rep.first_damaged[i]; }
    } else if (rc != XOLA_OK || r->dlen != want) {
        free(r->data); r->data = NULL; r->dlen = 0;
        if (rc == XOLA_E_MEM) return LITE_E_MEM;
        r->cur = k; r->cur_all_bad = 1;          /* nothing of this piece can be trusted */
        return LITE_OK;
    }
    r->cur = k;
    return LITE_OK;
}

static int range_bad(const lite_reader *r, uint64_t a, uint64_t b) {   /* [a, b) inside the cached piece */
    if (r->cur_all_bad) return 1;
    for (uint32_t i = 0; i < r->nbad; i++) {
        uint64_t s = r->badblk[i] * r->blk, e = s + r->blk;
        if (a < e && s < b) return 1;
    }
    return 0;
}

/* the bytes entry i takes in the stream (for an opened entry: its open form), checked against their CRC-32C;
 * w == NULL and ob != NULL: gathered into *ob */
static int stream_bytes(lite_reader *r, const lite_entry *t, lite_write_fn w, void *wctx, uint8_t *ob) {
    uint64_t pos = t->offset, end = t->offset + t->stored;
    uint32_t crc = 0; size_t on = 0;
    while (pos < end) {
        uint32_t k = (uint32_t)(pos / r->piece_size);
        int rc = load_piece(r, k);
        if (rc) return rc;
        uint64_t base = (uint64_t)k * r->piece_size;
        uint64_t a = pos - base, b = end - base; if (b > r->piece_size) b = r->piece_size;
        if (range_bad(r, a, b)) return LITE_E_DAMAGED;
        crc = xola_crc32c(crc, r->data + a, (size_t)(b - a));
        if (ob) { memcpy(ob + on, r->data + a, (size_t)(b - a)); on += (size_t)(b - a); }
        else if (w && w(wctx, r->data + a, (size_t)(b - a))) return LITE_E_IO;
        pos = base + b;
    }
    return crc == t->stored_crc ? LITE_OK : LITE_E_DAMAGED;
}

int lite_read_stored(lite_reader *r, uint32_t i, uint8_t **buf, size_t *n) {
    *buf = NULL; *n = 0;
    if (i >= r->ne || r->ents[i].is_dir) return LITE_E_ARG;
    const lite_entry *t = &r->ents[i];
    if (t->stored > ((uint64_t)1 << 31)) return LITE_E_DAMAGED;
    uint8_t *ob = malloc(t->stored ? (size_t)t->stored : 1);
    if (!ob) return LITE_E_MEM;
    int rc = stream_bytes(r, t, NULL, NULL, ob);
    if (rc) { free(ob); return rc; }
    *buf = ob; *n = (size_t)t->stored; return LITE_OK;
}

int lite_extract(lite_reader *r, uint32_t i, lite_write_fn w, void *wctx) {
    if (i >= r->ne) return LITE_E_ARG;
    const lite_entry *t = &r->ents[i];
    if (t->is_dir) return LITE_OK;
    if (!t->opened) return stream_bytes(r, t, w, wctx, NULL);
    lite_restore_job J; memset(&J, 0, sizeof J);   /* an opened file: its stored bytes are gathered, then restored */
    int rc = lite_read_stored(r, i, &J.stored, &J.sn);
    if (rc) return rc;
    J.size = t->size; J.crc = t->crc;
    lite_restore_many(&J, 1, r->threads);
    free(J.stored);
    if (J.rc) return J.rc;
    rc = w && J.on && w(wctx, J.out, J.on) ? LITE_E_IO : LITE_OK;
    free(J.out);
    return rc;
}

typedef struct { uint64_t off; uint32_t i; } ord_t;
static int by_off(const void *a, const void *b) {
    const ord_t *x = a, *y = b;
    return x->off != y->off ? (x->off < y->off ? -1 : 1) : (x->i < y->i ? -1 : x->i > y->i);
}
/* opened files waiting to be checked together; a copy of one already waiting takes its result */
typedef struct { lite_restore_job *job; uint32_t *ent; int *of; int nj, ne; uint64_t bytes; } tq_t;
static int tq_flush(lite_reader *r, tq_t *Q, void (*bad)(void *, uint32_t), void *ctx, int *nb) {
    lite_restore_many(Q->job, Q->nj, r->threads);
    int err = 0;
    for (int k = 0; k < Q->ne; k++) {
        int rc = Q->job[Q->of[k]].rc;
        if (rc == LITE_E_DAMAGED) { (*nb)++; if (bad) bad(ctx, Q->ent[k]); } else if (rc) err = rc;
    }
    for (int j = 0; j < Q->nj; j++) { free(Q->job[j].stored); free(Q->job[j].out); }
    Q->nj = Q->ne = 0; Q->bytes = 0;
    return err;
}
#define TQ_MAX 64
#define TQ_BYTES ((uint64_t)512 << 20)
int lite_test(lite_reader *r, void (*bad)(void *, uint32_t), void *ctx) {
    /* in the order the bytes lie in the archive, so each piece is unpacked once (copies point back to earlier bytes);
     * opened files are restored several at once on all cores */
    ord_t *o = malloc(sizeof *o * (r->ne ? r->ne : 1));
    tq_t Q; memset(&Q, 0, sizeof Q);
    Q.job = calloc(TQ_MAX, sizeof *Q.job); Q.ent = malloc(TQ_MAX * sizeof *Q.ent); Q.of = malloc(TQ_MAX * sizeof *Q.of);
    if (!o || !Q.job || !Q.ent || !Q.of) { free(o); free(Q.job); free(Q.ent); free(Q.of); return LITE_E_MEM; }
    int cap = 2 * r->threads; if (cap < 2) cap = 2; if (cap > TQ_MAX) cap = TQ_MAX;
    for (uint32_t i = 0; i < r->ne; i++) { o[i].off = r->ents[i].offset; o[i].i = i; }
    qsort(o, r->ne, sizeof *o, by_off);
    int nb = 0, err = 0;
    for (uint32_t j = 0; j < r->ne && !err; j++) {
        uint32_t i = o[j].i; const lite_entry *t = &r->ents[i];
        if (t->is_dir) continue;
        if (!t->opened) {
            int rc = lite_extract(r, i, NULL, NULL);
            if (rc == LITE_E_DAMAGED) { nb++; if (bad) bad(ctx, i); }
            else if (rc) err = rc;
            continue;
        }
        int same = -1;
        for (int k = 0; k < Q.ne && same < 0; k++) {
            const lite_entry *u = &r->ents[Q.ent[k]];
            if (u->offset == t->offset && u->stored == t->stored && u->stored_crc == t->stored_crc && u->size == t->size && u->crc == t->crc) same = Q.of[k];
        }
        if (same < 0) {
            if ((Q.nj == cap || Q.bytes + t->size > TQ_BYTES) && (err = tq_flush(r, &Q, bad, ctx, &nb))) break;
            lite_restore_job *J = &Q.job[Q.nj]; memset(J, 0, sizeof *J);
            int rc = lite_read_stored(r, i, &J->stored, &J->sn);
            if (rc == LITE_E_DAMAGED) { nb++; if (bad) bad(ctx, i); continue; }
            if (rc) { err = rc; break; }
            J->size = t->size; J->crc = t->crc; same = Q.nj++; Q.bytes += t->size;
        }
        Q.ent[Q.ne] = i; Q.of[Q.ne] = same; Q.ne++;
        if (Q.ne == TQ_MAX) err = tq_flush(r, &Q, bad, ctx, &nb);
    }
    if (!err) err = tq_flush(r, &Q, bad, ctx, &nb);
    else for (int k = 0; k < Q.nj; k++) free(Q.job[k].stored);
    free(o); free(Q.job); free(Q.ent); free(Q.of);
    if (err) return err;
    return nb;
}

void lite_close(lite_reader *r) {
    if (!r) return;
    free(r->pc); free(r->ents); free(r->names); free(r->data); free(r);
}
