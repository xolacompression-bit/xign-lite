/* The LZ coder (way A) - a seekable, block-parallel, static-table range coder.
 * Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE).
 * Where every idea comes from: docs/SOURCES.md. */
#ifndef XOLA_H
#define XOLA_H
#include <stddef.h>
#include <stdint.h>

#define XOLA_VERSION_STRING "0.3.1"

enum {
    XOLA_OK = 0,
    XOLA_E_MEM = -1,        /* out of memory */
    XOLA_E_FORMAT = -2,     /* not a Xola archive, or its header / index is damaged */
    XOLA_E_DAMAGED = -3,    /* one or more blocks failed their check (see xola_report) */
    XOLA_E_ARG = -4,        /* bad argument */
    XOLA_E_IO = -5
};

typedef struct {
    uint32_t block_kb;   /* block size in KiB, 64..16384 (default 4096) */
    int lanes;           /* coder lanes per block, 1..8 (default 4) */
    int threads;         /* worker threads (default 1); the archive never depends on it */
    int level;           /* 1..9: 1-5 fast, 6 the cheapest path (default), 7 the cheapest path searched twice (8 and 9 = 7) */
} xola_params;

void xola_default_params(xola_params *p);

/* Pack src[0..n) into a new buffer *out (free() it). */
int xola_pack(const uint8_t *src, size_t n, const xola_params *p, uint8_t **out, size_t *out_n);

/* Restore a whole archive. *out is malloc'd. On XOLA_E_DAMAGED the good blocks are still
 * restored (damaged ones are zero-filled) and xola_report lists the damaged blocks. */
typedef struct {
    uint32_t nblocks, ndamaged;
    uint32_t first_damaged[16];   /* the first damaged block numbers */
    uint64_t block_size;
} xola_report;
int xola_unpack(const uint8_t *arc, size_t n, int threads, uint8_t **out, size_t *out_n, xola_report *rep);

/* Restore only bytes [from, from+len) of the original: only the blocks that hold them are decoded. */
int xola_unpack_range(const uint8_t *arc, size_t n, uint64_t from, uint64_t len, uint8_t **out, size_t *out_n,
                      xola_report *rep);

/* Header facts without decoding. */
typedef struct { uint64_t orig_size; uint32_t block_size, nblocks; int groups, lanes; } xola_info_t;
int xola_info(const uint8_t *arc, size_t n, xola_info_t *info);

uint32_t xola_crc32c(uint32_t crc, const uint8_t *p, size_t n);

#endif
