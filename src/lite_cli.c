/* xola-lite - the command-line archiver of the Lite edition.
 * Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
#define _FILE_OFFSET_BITS 64
#include "lite.h"
#include "lopen.h"
#include "xola.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <direct.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#endif

#define PROG "xign-lite"

/* ------------------------------------------------------------------ files by UTF-8 name, on every system */
typedef struct { int ok, is_dir, is_reg; uint64_t size; int64_t mtime; uint64_t dev, ino; } fst;

#ifdef _WIN32
static wchar_t *u2w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) { MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); for (wchar_t *q = w; *q; q++) if (*q == L'/') *q = L'\\'; }
    return w;
}
static char *w2u(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? malloc((size_t)n) : NULL;
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
static FILE *ufopen(const char *u, const char *mode) {
    wchar_t *w = u2w(u), wm[8]; FILE *f = NULL;
    for (int i = 0; i < 7 && (wm[i] = (wchar_t)mode[i]); i++) wm[i + 1] = 0;
    if (w) f = _wfopen(w, wm);
    free(w); return f;
}
static fst ustat(const char *u) {
    fst r = {0}; wchar_t *w = u2w(u); struct _stat64 s;
    if (w && _wstat64(w, &s) == 0) {
        r.ok = 1; r.is_dir = (s.st_mode & _S_IFDIR) != 0; r.is_reg = (s.st_mode & _S_IFREG) != 0;
        r.size = (uint64_t)s.st_size; r.mtime = s.st_mtime;
        DWORD a = GetFileAttributesW(w);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) r.is_dir = r.is_reg = 0;   /* links are skipped */
    }
    free(w); return r;
}
static int umkdir(const char *u) { wchar_t *w = u2w(u); int rc = w ? _wmkdir(w) : -1; free(w); return rc; }
static int uremove(const char *u) { wchar_t *w = u2w(u); int rc = w ? _wremove(w) : -1; free(w); return rc; }
static int urename(const char *a, const char *b) {
    wchar_t *x = u2w(a), *y = u2w(b);
    int rc = (x && y && MoveFileExW(x, y, MOVEFILE_REPLACE_EXISTING)) ? 0 : -1;
    free(x); free(y); return rc;
}
static void usettime(const char *u, int64_t t) { wchar_t *w = u2w(u); struct __utimbuf64 b = {t, t}; if (w) _wutime64(w, &b); free(w); }
static int same_file(const char *a, const char *b) {
    wchar_t *x = u2w(a), *y = u2w(b), fx[4096], fy[4096]; int r = 0;
    if (x && y && _wfullpath(fx, x, 4096) && _wfullpath(fy, y, 4096)) r = _wcsicmp(fx, fy) == 0;
    free(x); free(y); return r;
}
/* the names inside a folder, UTF-8, malloc'd array */
static char **ulist(const char *dir, size_t *n) {
    size_t L = strlen(dir); char *pat = malloc(L + 3); char **v = NULL; size_t c = 0, cap = 0; *n = 0;
    if (!pat) return NULL;
    memcpy(pat, dir, L); strcpy(pat + L, "/*");
    wchar_t *w = u2w(pat); free(pat);
    WIN32_FIND_DATAW fd; HANDLE h = w ? FindFirstFileW(w, &fd) : INVALID_HANDLE_VALUE; free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (c == cap) { cap = cap ? cap * 2 : 64; char **q = realloc(v, cap * sizeof *q); if (!q) break; v = q; }
        v[c] = w2u(fd.cFileName); if (v[c]) c++;
    } while (FindNextFileW(h, &fd));
    FindClose(h); *n = c; return v;
}
static double now_s(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
static int fseek64(FILE *f, uint64_t o) { return _fseeki64(f, (__int64)o, SEEK_SET); }
static uint64_t ftell64(FILE *f) { return (uint64_t)_ftelli64(f); }
#else
static FILE *ufopen(const char *u, const char *mode) { return fopen(u, mode); }
static fst ustat(const char *u) {
    fst r = {0}; struct stat s;
    if (lstat(u, &s) == 0) {
        r.ok = 1; r.is_dir = S_ISDIR(s.st_mode); r.is_reg = S_ISREG(s.st_mode);   /* links: neither (skipped) */
        r.size = (uint64_t)s.st_size; r.mtime = s.st_mtime; r.dev = s.st_dev; r.ino = s.st_ino;
    }
    return r;
}
static int umkdir(const char *u) { return mkdir(u, 0777); }
static int uremove(const char *u) { return remove(u); }
static int urename(const char *a, const char *b) { return rename(a, b); }
static void usettime(const char *u, int64_t t) { struct utimbuf b; b.actime = (time_t)t; b.modtime = (time_t)t; utime(u, &b); }
static int same_file(const char *a, const char *b) {
    struct stat x, y;
    return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
static char **ulist(const char *dir, size_t *n) {
    DIR *d = opendir(dir); char **v = NULL; size_t c = 0, cap = 0; *n = 0;
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (c == cap) { cap = cap ? cap * 2 : 64; char **q = realloc(v, cap * sizeof *q); if (!q) break; v = q; }
        v[c] = strdup(e->d_name); if (v[c]) c++;
    }
    closedir(d); *n = c; return v;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int fseek64(FILE *f, uint64_t o) { return fseeko(f, (off_t)o, SEEK_SET); }
static uint64_t ftell64(FILE *f) { return (uint64_t)ftello(f); }
#endif

static char *join(const char *a, const char *b) {
    size_t x = strlen(a), y = strlen(b); char *s = malloc(x + y + 2);
    if (!s) return NULL;
    memcpy(s, a, x); s[x] = '/'; memcpy(s + x + 1, b, y + 1); return s;
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if ((*q == '/' || *q == '\\') && q[1]) b = q + 1;
    return b;
}

/* ------------------------------------------------------------------ pack */
typedef struct { FILE *f; } wctx_t;
static int fwrite_cb(void *c, const uint8_t *p, size_t n) { return fwrite(p, 1, n, ((wctx_t *)c)->f) == n ? 0 : -1; }
static int64_t fpread_cb(void *c, uint8_t *p, size_t n, uint64_t off) {
    FILE *f = c; if (fseek64(f, off)) return -1;
    size_t k = fread(p, 1, n, f); return k ? (int64_t)k : -1;
}

#define RBUF (1u << 20)
static int g_open = 1;   /* --no-open: documents stored as they are (for comparisons) */
/* copies stored once: every packed file is remembered by its size; a later file of the same size is compared byte for byte
 * with up to 8 earlier ones, and when all its bytes are equal it is stored as a copy of that file */
typedef struct { uint64_t size; uint32_t entry; char *path; } seen_t;
/* files to be opened (documents, PNGs, JPEGs up to 64 MB) wait here and are opened several at once on all cores; anything
 * else first lets the waiting files go into the archive, so every file keeps its place and the archive is the same bytes */
typedef struct { char *name, *path; int64_t mtime; uint8_t *data; size_t n; } qitem_t;
#define Q_MAX 64
#define Q_BYTES ((size_t)256 << 20)
#define Q_ONE ((uint64_t)64 << 20)       /* a file bigger than this is opened on its own, with all cores */
typedef struct { lite_packer *pk; const char *out; uint8_t *buf; int errors; seen_t *seen; size_t nseen, capseen; uint32_t copies;
                 uint64_t copy_bytes; uint8_t *cmpbuf; int threads; qitem_t q[Q_MAX]; int nq; size_t qbytes; } packrun;
#define COPY_MIN 4096

static int same_bytes(const char *a, const char *b, uint8_t *ba, uint8_t *bb) {
    FILE *x = ufopen(a, "rb"), *y = x ? ufopen(b, "rb") : NULL; int same = x && y;
    while (same) {
        size_t m = fread(ba, 1, RBUF / 2, x), k = fread(bb, 1, RBUF / 2, y);
        if (m != k || memcmp(ba, bb, m)) same = 0;
        if (!m) break;
    }
    if (x && ferror(x)) same = 0;
    if (y && ferror(y)) same = 0;
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}
static int find_copy(packrun *R, const char *path, uint64_t size, uint32_t *entry) {
    if (size < COPY_MIN || !R->cmpbuf) return 0;
    int tried = 0;
    for (size_t i = 0; i < R->nseen && tried < 8; i++) {
        if (R->seen[i].size != size) continue;
        tried++;
        if (same_bytes(path, R->seen[i].path, R->cmpbuf, R->cmpbuf + RBUF / 2)) { *entry = R->seen[i].entry; return 1; }
    }
    return 0;
}
static void remember(packrun *R, const char *path, uint64_t size, uint32_t entry) {
    if (size < COPY_MIN) return;
    if (R->nseen == R->capseen) {
        size_t c = R->capseen ? R->capseen * 2 : 256; seen_t *q = realloc(R->seen, c * sizeof *q);
        if (!q) return;
        R->seen = q; R->capseen = c;
    }
    char *p = strdup(path); if (!p) return;
    R->seen[R->nseen].size = size; R->seen[R->nseen].entry = entry; R->seen[R->nseen].path = p; R->nseen++;
}

static void flush_q(packrun *R) {
    if (!R->nq) return;
    lite_open_job J[Q_MAX]; memset(J, 0, sizeof J);
    for (int k = 0; k < R->nq; k++) { J[k].p = R->q[k].data; J[k].n = R->q[k].n; }
    lite_open_many(J, R->nq, R->threads);
    for (int k = 0; k < R->nq; k++) {
        qitem_t *q = &R->q[k];
        uint32_t me = lite_pack_count(R->pk);
        int rc = J[k].rc == 0 ? lite_pack_opened(R->pk, q->name, q->mtime, q->n, J[k].crc, J[k].out, J[k].on)
                              : lite_pack_file(R->pk, q->name, q->mtime, q->data, q->n, 0);
        if (rc) { fprintf(stderr, "   name not allowed, skipped: %s\n", q->path); R->errors++; }
        else remember(R, q->path, q->n, me);
        free(J[k].out); free(q->data); free(q->name); free(q->path);
    }
    R->nq = 0; R->qbytes = 0;
}

static void add_path(packrun *R, const char *path, const char *name, int depth) {
    fst s = ustat(path);
    if (!s.ok) { fprintf(stderr, "   cannot read, skipped: %s\n", path); R->errors++; return; }
    if (same_file(path, R->out)) return;                    /* never the archive itself */
    if (s.is_dir) {
        flush_q(R);
        if (depth > 64) { fprintf(stderr, "   too deep, skipped: %s\n", path); R->errors++; return; }
        if (lite_pack_entry(R->pk, name, s.mtime, 1)) { fprintf(stderr, "   name not allowed, skipped: %s\n", path); R->errors++; return; }
        size_t n = 0; char **v = ulist(path, &n);
        if (v) qsort(v, n, sizeof *v, cmpstr);
        for (size_t i = 0; i < n; i++) {
            char *p = join(path, v[i]), *nm = join(name, v[i]);
            if (p && nm) add_path(R, p, nm, depth + 1);
            free(p); free(nm); free(v[i]);
        }
        free(v);
        return;
    }
    if (!s.is_reg) return;                                  /* links, devices: skipped */
    for (int k = 0; k < R->nq; k++) if (s.size >= COPY_MIN && R->q[k].n == s.size) { flush_q(R); break; }   /* maybe its copy */
    uint32_t first;
    if (find_copy(R, path, s.size, &first)) {
        flush_q(R);
        if (lite_pack_copy(R->pk, name, s.mtime, first)) { fprintf(stderr, "   name not allowed, skipped: %s\n", path); R->errors++; }
        else { R->copies++; R->copy_bytes += s.size; }
        return;
    }
    FILE *f = ufopen(path, "rb");
    if (!f) { fprintf(stderr, "   cannot open, skipped: %s\n", path); R->errors++; return; }
    if (g_open && s.size >= 64 && s.size <= ((uint64_t)256 << 20)) {        /* a document: given whole, its packed parts opened */
        uint8_t sig[8]; size_t k0 = fread(sig, 1, 8, f);
        if (k0 == 8 && lopen_wanted(sig, 8)) {
            uint8_t *all = malloc((size_t)s.size);
            if (all) {
                memcpy(all, sig, 8);
                size_t got = 8 + fread(all + 8, 1, (size_t)s.size - 8, f);
                if (got != s.size || ferror(f)) { fprintf(stderr, "   read error: %s\n", path); R->errors++; free(all); fclose(f); return; }
                fclose(f);
                if (s.size <= Q_ONE) {                      /* waits, to be opened with the others */
                    char *nm = strdup(name), *pt = strdup(path);
                    if (nm && pt) {
                        if (R->nq == Q_MAX || R->qbytes + got > Q_BYTES) flush_q(R);
                        qitem_t *q = &R->q[R->nq++];
                        q->name = nm; q->path = pt; q->mtime = s.mtime; q->data = all; q->n = got; R->qbytes += got;
                        if (R->nq >= 2 * R->threads || R->nq == Q_MAX) flush_q(R);
                        return;
                    }
                    free(nm); free(pt);
                }
                flush_q(R);
                uint32_t me = lite_pack_count(R->pk);
                if (lite_pack_file(R->pk, name, s.mtime, all, got, 1)) { fprintf(stderr, "   name not allowed, skipped: %s\n", path); R->errors++; }
                else remember(R, path, s.size, me);
                free(all); return;
            }
        }
        rewind(f);
    }
    flush_q(R);
    uint32_t me = lite_pack_count(R->pk);
    if (lite_pack_entry(R->pk, name, s.mtime, 0)) { fprintf(stderr, "   name not allowed, skipped: %s\n", path); R->errors++; fclose(f); return; }
    size_t k;
    while ((k = fread(R->buf, 1, RBUF, f)) > 0) if (lite_pack_data(R->pk, R->buf, k)) break;
    if (ferror(f)) { fprintf(stderr, "   read error: %s\n", path); R->errors++; }
    else remember(R, path, s.size, me);
    fclose(f);
}

static int cmd_pack(const char *out, char **paths, int np, const lite_opts *o) {
    size_t L = strlen(out); char *tmp = malloc(L + 6);
    if (!tmp) return 1;
    memcpy(tmp, out, L); strcpy(tmp + L, ".part");
    FILE *f = ufopen(tmp, "wb");
    if (!f) { fprintf(stderr, PROG ": cannot write %s\n", out); free(tmp); return 1; }
    wctx_t wc = {f}; int err = 0;
    double t0 = now_s();
    packrun R = {lite_pack_begin(fwrite_cb, &wc, o, &err), out, malloc(RBUF), 0, NULL, 0, 0, 0, 0, malloc(RBUF), o->threads};
    lite_stats st = {0};
    int rc = err;
    if (R.pk && R.buf) {
        for (int i = 0; i < np; i++) {
            char *p = strdup(paths[i]);
            size_t m = strlen(p); while (m > 1 && (p[m - 1] == '/' || p[m - 1] == '\\')) p[--m] = 0;
            add_path(&R, p, base_name(p), 0);
            free(p);
        }
        flush_q(&R);
        rc = lite_pack_end(R.pk, &st);
    } else if (!rc) rc = LITE_E_MEM;
    double t = now_s() - t0;
    if (fclose(f)) rc = rc ? rc : LITE_E_IO;
    lite_pack_free(R.pk); free(R.buf); free(R.cmpbuf);
    for (size_t i = 0; i < R.nseen; i++) free(R.seen[i].path);
    free(R.seen);
    if (rc || urename(tmp, out)) {
        uremove(tmp); free(tmp);
        fprintf(stderr, PROG ": FAILED (%s) - nothing was written\n", lite_strerror(rc ? rc : LITE_E_IO));
        return 1;
    }
    free(tmp);
    double pct = st.in_bytes ? 100.0 - 100.0 * (double)st.out_bytes / (double)st.in_bytes : 0;
    printf("%u files, %u folders: %.2f MB -> %.2f MB  %.2f%% smaller  pack %.1f MB/s\n", st.files, st.folders,
           st.in_bytes / 1e6, st.out_bytes / 1e6, pct, t > 0 ? st.in_bytes / 1e6 / t : 0);
    if (st.opened) printf("%u file(s) stored open - documents, PNGs and JPEGs, restored byte for byte\n", st.opened);
    if (R.copies) printf("%u file(s) were exact copies of an earlier file, stored once (%.2f MB)\n", R.copies, R.copy_bytes / 1e6);
    if (R.errors) printf("%d item(s) skipped (listed above)\n", R.errors);
    return R.errors ? 2 : 0;
}

/* ------------------------------------------------------------------ read */
static lite_reader *open_arc(const char *path, FILE **fo, int threads) {
    FILE *f = ufopen(path, "rb");
    if (!f) { fprintf(stderr, PROG ": cannot open %s\n", path); return NULL; }
    uint64_t n = 0;
    if (!fseek(f, 0, SEEK_END)) n = ftell64(f);
    int err = 0;
    lite_reader *r = lite_open(fpread_cb, f, n, threads, &err);
    if (!r) { fprintf(stderr, PROG ": %s: %s\n", path, lite_strerror(err)); fclose(f); return NULL; }
    *fo = f; return r;
}

static int cmd_list(const char *arc) {
    FILE *f; lite_reader *r = open_arc(arc, &f, 1);
    if (!r) return 1;
    uint64_t tot = 0;
    for (uint32_t i = 0; i < lite_count(r); i++) {
        const lite_entry *e = lite_get(r, i);
        char tb[32] = "-"; time_t tt = (time_t)e->mtime; struct tm *tm = gmtime(&tt);
        if (tm) strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", tm);
        if (e->is_dir) printf("%14s  %s  %s/\n", "<folder>", tb, e->name);
        else { printf("%14llu  %s  %s\n", (unsigned long long)e->size, tb, e->name); tot += e->size; }
    }
    printf("%u entries, %llu bytes\n", lite_count(r), (unsigned long long)tot);
    lite_close(r); fclose(f); return 0;
}

static void bad_name(void *c, uint32_t i) { lite_reader *r = c; printf("   DAMAGED: %s\n", lite_get(r, i)->name); }

static int cmd_test(const char *arc, int threads) {
    FILE *f; lite_reader *r = open_arc(arc, &f, threads);
    if (!r) return 1;
    double t0 = now_s();
    int nb = lite_test(r, bad_name, r);
    double t = now_s() - t0;
    if (nb < 0) printf("FAILED: %s\n", lite_strerror(nb));
    else if (nb) printf("%d of %u entries DAMAGED (named above); the others are good\n", nb, lite_count(r));
    else printf("all %u entries good (every file checked)  restore %.1f MB/s\n", lite_count(r),
                t > 0 ? lite_stream_size(r) / 1e6 / t : 0);
    lite_close(r); fclose(f);
    return nb < 0 ? 1 : nb ? 3 : 0;
}

static int mkdirs(const char *path) {   /* make every folder of path (not the last part) */
    char *p = strdup(path); if (!p) return -1;
    for (char *q = p + 1; *q; q++) if (*q == '/' || *q == '\\') { char k = *q; *q = 0; fst s = ustat(p); if (!s.ok) umkdir(p); *q = k; }
    free(p); return 0;
}

/* never overwrite a file in place: a temporary file is written, and renamed only after the file came back exact */
static char *tmp_name(const char *dest) {
    size_t L = strlen(dest); char *tmp = malloc(L + 12);
    if (tmp) { memcpy(tmp, dest, L); strcpy(tmp + L, ".xlite-part"); }
    return tmp;
}
static int finish_one(const lite_entry *e, const char *dest, char *tmp, int rc, int *bad) {
    if (!rc) { usettime(tmp, e->mtime); if (urename(tmp, dest)) rc = LITE_E_IO; }
    if (rc) { uremove(tmp); free(tmp);
              if (rc == LITE_E_DAMAGED) { printf("   DAMAGED, not written: %s\n", e->name); (*bad)++; return 0; }
              fprintf(stderr, "   FAILED (%s): %s\n", lite_strerror(rc), e->name); return 1; }
    free(tmp);
    return 0;
}
static int write_one(lite_reader *r, uint32_t i, const char *dest, int *bad) {
    const lite_entry *e = lite_get(r, i);
    if (e->is_dir) { mkdirs(dest); fst s = ustat(dest); if (!s.ok) umkdir(dest); return 0; }
    mkdirs(dest);
    char *tmp = tmp_name(dest);
    if (!tmp) return 1;
    FILE *o = ufopen(tmp, "wb");
    if (!o) { fprintf(stderr, "   cannot write: %s\n", dest); free(tmp); return 1; }
    wctx_t wc = {o};
    int rc = lite_extract(r, i, fwrite_cb, &wc);
    if (fclose(o)) rc = rc ? rc : LITE_E_IO;
    return finish_one(e, dest, tmp, rc, bad);
}
static int write_bytes(const lite_entry *e, const char *dest, const uint8_t *p, size_t n, int bad_rc, int *bad) {
    mkdirs(dest);
    char *tmp = tmp_name(dest);
    if (!tmp) return 1;
    FILE *o = ufopen(tmp, "wb");
    if (!o) { fprintf(stderr, "   cannot write: %s\n", dest); free(tmp); return 1; }
    int rc = bad_rc ? bad_rc : (n && fwrite(p, 1, n, o) != n) ? LITE_E_IO : LITE_OK;
    if (fclose(o)) rc = rc ? rc : LITE_E_IO;
    return finish_one(e, dest, tmp, rc, bad);
}

/* opened files (documents, PNGs, JPEGs) waiting to be restored together on all cores; a copy of one already waiting
 * takes its result */
#define XQ_MAX 64
#define XQ_BYTES ((uint64_t)512 << 20)
typedef struct { lite_restore_job job[XQ_MAX]; uint32_t ent[XQ_MAX]; char *dest[XQ_MAX]; int of[XQ_MAX]; int nj, ne, cap; uint64_t bytes; } xq_t;
static int xq_flush(lite_reader *r, int threads, xq_t *Q, int *bad) {
    lite_restore_many(Q->job, Q->nj, threads);
    int fails = 0;
    for (int k = 0; k < Q->ne; k++) {
        const lite_restore_job *J = &Q->job[Q->of[k]];
        fails += write_bytes(lite_get(r, Q->ent[k]), Q->dest[k], J->out, J->on, J->rc, bad);
        free(Q->dest[k]);
    }
    for (int j = 0; j < Q->nj; j++) { free(Q->job[j].stored); free(Q->job[j].out); }
    Q->nj = Q->ne = 0; Q->bytes = 0;
    return fails;
}
/* entry i, to dest (dest is kept by the queue) */
static int xq_add(lite_reader *r, int threads, xq_t *Q, uint32_t i, char *dest, int *bad) {
    const lite_entry *t = lite_get(r, i);
    int fails = 0, same = -1;
    for (int k = 0; k < Q->ne && same < 0; k++) {
        const lite_entry *u = lite_get(r, Q->ent[k]);
        if (u->offset == t->offset && u->stored == t->stored && u->stored_crc == t->stored_crc && u->size == t->size && u->crc == t->crc) same = Q->of[k];
    }
    if (same < 0) {
        if (Q->nj == Q->cap || Q->bytes + t->size > XQ_BYTES) fails += xq_flush(r, threads, Q, bad);
        lite_restore_job *J = &Q->job[Q->nj]; memset(J, 0, sizeof *J);
        int rc = lite_read_stored(r, i, &J->stored, &J->sn);
        if (rc) { fails += write_bytes(t, dest, NULL, 0, rc, bad); free(dest); return fails; }
        J->size = t->size; J->crc = t->crc; same = Q->nj++; Q->bytes += t->size;
    }
    Q->ent[Q->ne] = i; Q->dest[Q->ne] = dest; Q->of[Q->ne] = same; Q->ne++;
    if (Q->ne == XQ_MAX) fails += xq_flush(r, threads, Q, bad);
    return fails;
}

static lite_reader *sort_r;
static int by_offset(const void *a, const void *b) {
    const lite_entry *x = lite_get(sort_r, *(const uint32_t *)a), *y = lite_get(sort_r, *(const uint32_t *)b);
    if (x->offset != y->offset) return x->offset < y->offset ? -1 : 1;
    uint32_t i = *(const uint32_t *)a, k = *(const uint32_t *)b; return i < k ? -1 : i > k;
}

static int cmd_extract(const char *arc, const char *dir, int threads, int overwrite) {
    FILE *f; lite_reader *r = open_arc(arc, &f, threads);
    if (!r) return 1;
    fst ds = ustat(dir); if (!ds.ok) { mkdirs(dir); umkdir(dir); }
    int fails = 0, bad = 0, skipped = 0;
    double t0 = now_s();
    /* folders first, then the files in the order their bytes lie in the archive, so each piece is unpacked once even when
     * a copy points back to an earlier piece */
    uint32_t ne = lite_count(r), *ord = malloc(sizeof *ord * (ne ? ne : 1)), no = 0;
    if (!ord) { lite_close(r); fclose(f); return 1; }
    for (uint32_t i = 0; i < ne; i++) if (lite_get(r, i)->is_dir) ord[no++] = i;
    uint32_t nd = no;
    for (uint32_t i = 0; i < ne; i++) if (!lite_get(r, i)->is_dir) ord[no++] = i;
    sort_r = r; qsort(ord + nd, no - nd, sizeof *ord, by_offset);
    xq_t *Q = calloc(1, sizeof *Q);
    if (!Q) { free(ord); lite_close(r); fclose(f); return 1; }
    Q->cap = 2 * threads < 2 ? 2 : 2 * threads > XQ_MAX ? XQ_MAX : 2 * threads;
    for (uint32_t j = 0; j < ne; j++) {
        uint32_t i = ord[j];
        const lite_entry *e = lite_get(r, i);
        char *dest = join(dir, e->name);
        if (!dest) { fails++; break; }
        fst s = ustat(dest);
        if (s.ok && !e->is_dir && !overwrite) { printf("   exists, kept: %s\n", dest); skipped++; free(dest); continue; }
        if (e->opened) { fails += xq_add(r, threads, Q, i, dest, &bad); continue; }
        fails += write_one(r, i, dest, &bad);
        free(dest);
    }
    fails += xq_flush(r, threads, Q, &bad);
    free(Q); free(ord);
    double t = now_s() - t0;
    printf("%u entries -> %s  restore %.1f MB/s", lite_count(r), dir, t > 0 ? lite_stream_size(r) / 1e6 / t : 0);
    if (bad) printf("  %d DAMAGED (named above, not written)", bad);
    if (skipped) printf("  %d kept (already there; -o to replace)", skipped);
    if (fails) printf("  %d FAILED", fails);
    printf("\n");
    lite_close(r); fclose(f);
    return fails ? 1 : bad ? 3 : 0;
}

static int cmd_one(const char *arc, const char *name, const char *out, int threads) {
    FILE *f; lite_reader *r = open_arc(arc, &f, threads);
    if (!r) return 1;
    int rc = 1, bad = 0, found = 0;
    for (uint32_t i = 0; i < lite_count(r) && !found; i++)
        if (!strcmp(lite_get(r, i)->name, name)) { found = 1; rc = write_one(r, i, out, &bad); if (!rc && !bad) printf("restored %s -> %s\n", name, out); }
    if (!found) fprintf(stderr, PROG ": %s is not in the archive\n", name);
    lite_close(r); fclose(f);
    return rc ? 1 : bad ? 3 : 0;
}

static void usage(void) {
    printf("Xign Lite %s - a lossless archiver\n\n"
           "  " PROG " a [-t N] [--no-open] ARCHIVE FILE-OR-FOLDER...   pack\n"
           "  " PROG " x [-t N] [-o] ARCHIVE [FOLDER]                 restore everything (-o: replace files already there)\n"
           "  " PROG " e [-t N] ARCHIVE NAME OUT-FILE                 restore one file\n"
           "  " PROG " l ARCHIVE                                      list\n"
           "  " PROG " t [-t N] ARCHIVE                               check every file\n\n"
           "Pack option --no-open: store documents and pictures as they are (no opening of their packed parts).\n"
           "Every file is checked when it is restored; a damaged file is named and not written.\n"
           "Exit code: 0 ok, 1 error, 2 some items skipped, 3 damaged files named.\n"
           "Copyright 2026 Ximon Paul M. Rodriguez. Apache License 2.0, provided \"AS IS\" without warranty.\n", LITE_VERSION_STRING);
}

static int run(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const char *c = argv[1];
    if (!strcmp(c, "version") || !strcmp(c, "-v") || !strcmp(c, "--version")) {
        printf(PROG " %s\nCopyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0.\n"
               "Provided \"AS IS\", without warranty. \"Xign\" is a trademark; see TRADEMARKS.md.\n", LITE_VERSION_STRING);
        return 0;
    }
    lite_opts o; lite_default_opts(&o);
    o.threads = 8; o.piece_mb = 128;                                 /* a computer: bigger pieces, more at once */
    int overwrite = 0, i = 2;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-o")) overwrite = 1;
        else if (!strcmp(argv[i], "--no-open")) g_open = 0;
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) { int m = atoi(argv[++i]); o.piece_mb = m < 1 ? 1 : m > 256 ? 256 : (uint32_t)m; }
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) { o.threads = atoi(argv[++i]); if (o.threads < 1) o.threads = 1; if (o.threads > 64) o.threads = 64; }
        else { usage(); return 1; }
    }
    int n = argc - i; char **a = argv + i;
    if (!strcmp(c, "a") && n >= 2) return cmd_pack(a[0], a + 1, n - 1, &o);
    if (!strcmp(c, "x") && (n == 1 || n == 2)) return cmd_extract(a[0], n == 2 ? a[1] : ".", o.threads, overwrite);
    if (!strcmp(c, "e") && n == 3) return cmd_one(a[0], a[1], a[2], o.threads);
    if (!strcmp(c, "l") && n == 1) return cmd_list(a[0]);
    if (!strcmp(c, "t") && n == 1) return cmd_test(a[0], o.threads);
    usage(); return 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wargv) {
    SetConsoleOutputCP(CP_UTF8);
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return 1;
    for (int i = 0; i < argc; i++) argv[i] = w2u(wargv[i]);
    int rc = run(argc, argv);
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv); return rc;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
