/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
/* ljpeg.h - Xign Lite: reading a baseline JPEG down to its DCT coefficients and writing back exactly the same bytes
 * (step J1, 2026-11-09). Written from the public JPEG standard (ISO/IEC 10918-1 / ITU-T T.81, 1992) only.
 *
 * Handled: sequential Huffman JPEGs (SOF0 baseline, SOF1 extended) with 8-bit samples and ONE scan holding every
 * component (or a grey JPEG), any sampling, restart markers, either padding bit, anything after the picture (phone extras).
 * Anything else (progressive, arithmetic, 12-bit, several scans, odd encoder choices) is reported as "not handled" and is
 * stored as it is - never made bigger, never changed.
 */
#ifndef LJPEG_H
#define LJPEG_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height, ncomp, nblocks, mcus, restart;  /* nblocks: 8x8 blocks in the scan; restart: MCUs per interval (0 = none) */
    int hs[4], vs[4];                                 /* sampling of each component */
    size_t head_len, scan_off, scan_len, tail_len;    /* bytes before the scan data, the scan data, bytes after it */
    int pad_bit;                                      /* the encoder's padding bit (1 or 0) */
    int per_mcu, blk_comp[10];                        /* blocks per MCU, and which scan component each one belongs to */
    int mcux, blk_x[10], blk_y[10];                   /* MCUs per row; each block's column / row inside its MCU (its part's grid) */
} ljpeg_info;

/* the reasons a JPEG is not handled (0 = handled) */
enum { LJ_OK = 0, LJ_NOT_JPEG, LJ_PROGRESSIVE, LJ_ARITHMETIC, LJ_NOT_8BIT, LJ_SCANS, LJ_DAMAGED, LJ_NOT_SAME, LJ_MEM, LJ_OTHER };
const char *ljpeg_reason(int code);

/* reads p as a JPEG: info, and the coefficients (nblocks x 64 int16, in coding order: each block's 64 values in
 * zig-zag order, the blocks in the order the scan codes them). The coefficients are then written back with the file's
 * own tables and checked to give exactly the same scan bytes. Returns LJ_OK or a reason. *coef is malloc'ed. */
int ljpeg_read(const uint8_t *p, size_t n, ljpeg_info *I, int16_t **coef);
/* writes the scan data for these coefficients with the tables in the header p[0 .. I->head_len): into *out (malloc'ed).
 * I gives head_len, pad_bit and the block layout the coefficients have (nblocks, per_mcu, mcux, blk_comp, blk_x, blk_y);
 * the header must give the same layout, or nothing is written (LJ_DAMAGED). */
int ljpeg_write_scan(const uint8_t *p, const ljpeg_info *I, const int16_t *coef, uint8_t **out, size_t *on);
#endif
