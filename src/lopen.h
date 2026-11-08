/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
/* lopen.h - Xign Lite: opening the packed parts inside documents (2026-11-08).
 *
 * A PDF keeps most of its text in packed ("deflate") streams; a DOCX / XLSX / PPTX / ODT / EPUB is a ZIP of packed parts.
 * Packed bytes look random, so a general coder can barely shrink them. lopen finds those parts, unpacks them, and keeps
 * the part only when packing it again with the bundled zlib 1.3.1 (one of its standard settings) gives back exactly the
 * same bytes. The file is then stored with those parts open (plain text that the coder shrinks well) plus a small table
 * of where each part was and its zlib setting. The restore packs each part again with that setting and gets back the
 * original file, byte for byte (the settings are tried, and the parts packed again, on all cores); every opened file is checked before it is accepted, and its CRC is checked on restore.
 * Public methods only: deflate (RFC 1951) / zlib (RFC 1950), ZIP local headers (PKWARE APPNOTE), PDF streams (ISO 32000).
 */
#ifndef LOPEN_H
#define LOPEN_H
#include <stddef.h>
#include <stdint.h>

/* 1 when the file looks like a document lopen can open (PDF or ZIP-based) */
int lopen_wanted(const uint8_t *p, size_t n);
/* opens what can be opened exactly. Returns 0 and a new buffer (*out, *on) when at least one part was opened; 1 when
 * nothing was opened (store the file as it is); -1 when out of memory. The result is checked by lopen_restore before
 * it is returned. */
int lopen_pack(const uint8_t *p, size_t n, int threads, uint8_t **out, size_t *on);
/* the original file from an opened one. 0 = ok; 1 = damaged; -1 = out of memory. */
int lopen_restore(const uint8_t *p, size_t n, int threads, uint8_t **out, size_t *on);
#endif
