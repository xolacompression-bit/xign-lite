/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
/* lcoef.h - Xign Lite: coding a JPEG's DCT coefficients.
 * Public base:
 *  - the JPEG standard's own arithmetic-coding decisions (ISO/IEC 10918-1 / ITU-T T.81, Annex F.1.4, 1992): DC zero /
 *    sign / size class / extra bits, its odds chosen by the previous DC difference of the same colour part;
 *  - the same place in the block above and the block to the left as a clue for each AC value: C. Tu and T. D. Tran,
 *    "Context-based entropy coding of block transform coefficients for image compression", IEEE Trans. Image Processing
 *    11(11), 2002; packJPG (M. Stirner, 2006): the number of non-zero AC values of a block coded first, with the
 *    neighbours' counts as the clue;
 *  - a binary range coder with 11-bit odds (G. N. N. Martin 1979; the public LZMA form).
 * Lite's own rules: DC coded first and never from the block's own AC; no above-left block; no edge / inner / top / left
 * class (a block at the picture's edge sees zeros where a neighbour is missing); the first-column places (vertical
 * frequencies) take no clue from outside the block; one writer; no fitted guesses; no copy check.
 * Lite's own code (Apache 2.0). */
#ifndef LCOEF_H
#define LCOEF_H
#include <stddef.h>
#include <stdint.h>
/* where the blocks lie: blocks per MCU, the colour part of each, its column / row inside the MCU, MCUs per row */
typedef struct { int per_mcu, mcux; int comp[10], bx[10], by[10]; } lcoef_layout;
/* blocks: nblocks x 64 int16 in coding order (zig-zag inside each block) */
int lcoef_encode(const int16_t *coef, int nblocks, const lcoef_layout *L, uint8_t **out, size_t *on);
int lcoef_decode(const uint8_t *in, size_t n, int nblocks, const lcoef_layout *L, int16_t *coef);
#endif
