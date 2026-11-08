# Where every part comes from

Xign Lite is built only from published, long-known methods. Every part is our own code, written from the sources below, except `src/zlib/` (an unchanged copy of zlib 1.3.1).

| part | file | public source |
|-|-|-|
| the archive: files one after another in pieces, an index at the end, a check per block and per file; one packing way and one setting for every archive | `src/lite.c` | ZIP's central directory (1989); solid blocks as in 7z; the indexed tar.xz of pixz (2009); xz / Zstandard seekable indexes |
| CRC-32C | `src/xola.c` | G. Castagnoli et al. (1993) |
| way A: match finder (chained hash, lazy matching) | `src/xola.c` | RFC 1951 (DEFLATE), section 4 |
| way A: three repeat distances | `src/xola.c` | LZX (J. Forbes, T. Poutanen, 1995) |
| way A: the cheapest path through a block | `src/xola.c` | Schuegraf & Heaps (1974); Storer & Szymanski (1982) |
| way A: two-pass frequency tables, one base set per file, block tables as differences | `src/xola.c` | semi-static coding (Huffman 1952); table reuse between blocks |
| way A: literals grouped by the previous byte | `src/xola.c` | context maps and clustering, RFC 7932 (Brotli) |
| way A: a block the matches do not shrink is coded again as plain bytes with flat tables (every byte value 1/256: exactly 8 bits a byte) - the same coder and format; no block is ever stored outside the coder (one method for every block of an archive) | `src/xola.c` | an LZ parse with no matches (RFC 1951 section 4); a flat table (Huffman 1952) |
| way A: tokens spread over lanes | `src/xola.c` | F. Giesen, "Interleaved entropy coders" (2014) |
| range coder | `src/xola.c` | G. N. N. Martin (1979); carry-less form, D. Subbotin (1999) |
| files that are exact copies stored once (same size, then every byte compared; no hash) | `src/lite_cli.c`, `src/lite.c` | single-instance storage (Microsoft SIS, filed 1999, expired); tar hard-link entries (1979) |
| documents: packed parts opened, packed again on restore with the same zlib setting, byte for byte | `src/lopen.c` | RFC 1950 / 1951 (zlib, DEFLATE, 1996); PKWARE ZIP APPNOTE (1989); ISO 32000 PDF streams; precomp (C. Schneider, 2006) - the bundled zlib 1.3.1 (`src/zlib/`, zlib License) does the packing |
| PNG: the picture data opened, packed again on restore by zlib 1.3.1 with the same setting, byte for byte, one writer; kept opened only when that packs smaller | `src/lopen.c` | ISO/IEC 15948 (PNG, 2003; IDAT chunks, CRC-32); RFC 1950 / 1951; precomp (2006) |
| JPEG: read down to its DCT coefficients and written back byte for byte (Huffman tables, restart markers, padding) | `src/ljpeg.c` | ISO/IEC 10918-1 / ITU-T T.81 (1992): B.1-B.2, C, F.1.2, F.2.2 |
| JPEG: the coefficients coded by binary decisions with adaptive odds - DC first, by the previous DC difference of the same colour part (the standard's model); the block's non-zero count by the neighbours' counts (zero or not, then its size class and bits, as the standard codes numbers); each AC value by the same place in the block above and to the left and by the place before it in the block (no above-left block, no edge class, first-column places without outside clues) | `src/lcoef.c` | ITU-T T.81 Annex F.1.4 (1992); C. Tu and T. D. Tran, IEEE Trans. Image Processing 11(11), 2002 (same-place clues from the left and top blocks); packJPG (M. Stirner, 2006: non-zero count first); binary range coder: G. N. N. Martin (1979), the public LZMA form |
