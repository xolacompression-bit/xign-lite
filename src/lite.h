/* Lite archive - many files and folders in one archive on top of the Xola coder.
 * Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE).
 *
 * The archive (all numbers little-endian):
 *   header   16 bytes: "LITEARC1", u32 version (2), u32 0
 *   pieces   the files' bytes one after another (one "solid" stream), cut into pieces of piece_size bytes;
 *            every piece is a complete Xola coder archive (xola.h, the LZ coder at Lite's one setting); each has its own
 *            block index and a CRC-32C per block; one method for the whole archive
 *   index    a Xola coder archive of the directory: the way every piece is packed (1 = LZ),
 *            piece offsets, then every entry
 *            (type, name, size, place in the stream, time, CRC-32C of the file)
 *   trailer  40 bytes: u64 index offset, u64 index length, u64 stream length, u32 CRC-32C of the 24 bytes before,
 *            u32 0, "LITEEND1"
 * Public prior art for the container: ZIP's central directory (1989), 7z solid blocks, pixz (indexed tar.xz, 2009),
 * xz / zstd seekable indexes. */
#ifndef LITE_H
#define LITE_H
#include <stddef.h>
#include <stdint.h>

#define LITE_VERSION_STRING "1.0.0"

enum { LITE_OK = 0, LITE_E_MEM = -1, LITE_E_FORMAT = -2, LITE_E_DAMAGED = -3, LITE_E_ARG = -4, LITE_E_IO = -5,
       LITE_E_STOPPED = -6 };

typedef int (*lite_write_fn)(void *ctx, const uint8_t *p, size_t n);              /* 0 = ok */
typedef int64_t (*lite_pread_fn)(void *ctx, uint8_t *p, size_t n, uint64_t off);   /* bytes read, < 0 = error */

enum { LITE_LZ = 1 };   /* the one packing way (written in the index) */
#define LITE_LEVEL 3     /* Lite's one setting of the LZ coder, for every archive */
typedef struct {
    int level;           /* kept for the app's bridge; Lite always packs at LITE_LEVEL */
    int threads;         /* default 4 */
    uint32_t piece_mb;   /* 1..256 (default 32) */
    uint32_t lz_block_mb;   /* 1..16 (default 4) */
} lite_opts;
void lite_default_opts(lite_opts *o);

/* ---- packing: entry, its data in any number of pushes, the next entry ..., end */
typedef struct lite_packer lite_packer;
lite_packer *lite_pack_begin(lite_write_fn w, void *wctx, const lite_opts *o, int *err);
int lite_pack_entry(lite_packer *pk, const char *name_utf8, int64_t mtime, int is_dir);
int lite_pack_data(lite_packer *pk, const uint8_t *p, size_t n);
/* a file with exactly the same bytes as an earlier file entry (same_as = that entry's number, from lite_pack_count before
 * it was added): stored once - the new entry points to the same bytes. The caller must have checked the bytes are equal;
 * the restore checks them again (CRC-32C). */
int lite_pack_copy(lite_packer *pk, const char *name_utf8, int64_t mtime, uint32_t same_as);
uint32_t lite_pack_count(const lite_packer *pk);   /* entries added so far */
/* a whole file given at once: when it is a document whose packed parts can be opened exactly (lopen.c), it is stored open
 * (smaller after packing) and restored to the same bytes; otherwise it is stored as it is. An archive with an opened file
 * is format 3. */
int lite_pack_file(lite_packer *pk, const char *name_utf8, int64_t mtime, const uint8_t *p, size_t n, int allow_open);
/* several whole files opened at once on all cores (each file still has its own single writer): fill p, n; get rc
 * (0 = opened: out/on is its open form, to be freed; 1 = not opened, store it as it is; -1 = out of memory) and crc
 * (CRC-32C of the file). The open forms are the same bytes whatever the number of cores. */
typedef struct { const uint8_t *p; size_t n; uint8_t *out; size_t on; uint32_t crc; int rc; } lite_open_job;
void lite_open_many(lite_open_job *jobs, int count, int threads);
/* a file the caller opened (lite_open_many, rc 0): stored open, the same as lite_pack_file would store it */
int lite_pack_opened(lite_packer *pk, const char *name_utf8, int64_t mtime, uint64_t size, uint32_t crc, const uint8_t *open_form, size_t open_len);
typedef struct { uint64_t in_bytes, out_bytes; uint32_t files, folders, pieces; int method; uint32_t opened; } lite_stats;   /* opened: documents stored open */
int lite_pack_end(lite_packer *pk, lite_stats *st);   /* writes the index and the trailer */
void lite_pack_free(lite_packer *pk);
volatile int *lite_pack_stop_flag(lite_packer *pk);    /* set to 1 from another thread to stop */

/* ---- reading */
typedef struct {
    const char *name;    /* UTF-8, folders separated by '/' */
    uint64_t size, offset;
    int64_t mtime;
    uint32_t crc;
    int is_dir;
    int opened;          /* 1: a document stored with its packed parts open (lopen.c); size and crc are the original file's */
    uint64_t stored;     /* bytes this entry takes in the stream (= size when not opened) */
    uint32_t stored_crc; /* CRC-32C of those bytes (= crc when not opened) */
} lite_entry;
typedef struct lite_reader lite_reader;
lite_reader *lite_open(lite_pread_fn rd, void *rctx, uint64_t arc_size, int threads, int *err);
uint32_t lite_count(const lite_reader *r);
const lite_entry *lite_get(const lite_reader *r, uint32_t i);
uint64_t lite_stream_size(const lite_reader *r);
int lite_method(const lite_reader *r);   /* LITE_LZ */
/* Write entry i's bytes. LITE_OK only if every byte came back and its CRC-32C matches;
 * LITE_E_DAMAGED: the file touches a damaged part of the archive (nothing more should be trusted). */
int lite_extract(lite_reader *r, uint32_t i, lite_write_fn w, void *wctx);
/* opened entries restored several at once on all cores: lite_read_stored gives an opened entry's stored bytes (checked
 * against their CRC-32C); put them in a job with the entry's size and crc, then lite_restore_many fills out/on and rc
 * (LITE_OK only when the file came back with its own size and CRC-32C; LITE_E_DAMAGED; LITE_E_MEM). The caller frees
 * stored and out. */
int lite_read_stored(lite_reader *r, uint32_t i, uint8_t **stored, size_t *n);
typedef struct { uint8_t *stored; size_t sn; uint64_t size; uint32_t crc; uint8_t *out; size_t on; int rc; } lite_restore_job;
void lite_restore_many(lite_restore_job *jobs, int count, int threads);
/* Check every piece and every file; calls bad(ctx, i) for each damaged entry. Returns the number of damaged entries
 * (0 = all good) or an error < 0. */
int lite_test(lite_reader *r, void (*bad)(void *ctx, uint32_t i), void *ctx);
void lite_close(lite_reader *r);

/* a safe relative name: no empty part, no ".", "..", no leading '/', no '\\', ':' or control characters */
int lite_name_safe(const char *name);
const char *lite_strerror(int e);
#endif
