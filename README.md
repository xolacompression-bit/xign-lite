# Xign Lite

A free, open-source, **lossless** archiver for **Android, Windows and Linux**. It packs files and folders into one
archive and gives every file back exactly, byte for byte.

| | |
|-|-|
| **Exact** | every file is checked when it is restored; a file that does not come back exactly is never written |
| **Damage is named** | if an archive was damaged, Xign Lite names the damaged files and still restores all the others |
| **One file at a time** | restore a single file from a large archive without unpacking everything |
| **Safe** | restoring never overwrites a file in place: it writes a temporary file and renames it only after the check passes |
| **Private** | it works only on the files you choose; it has no internet access and collects no data |
| **Any names** | file names in any language |

## How much smaller

Measured on the developer's PC (Windows 11, WSL, 32 threads) on 2026-11-09, with every file restored and compared
byte for byte. Each set was packed as one archive; copies of the same file were removed first. Your files will differ.

| kind of file | files | smaller by |
|-|-|-|
| camera DNG photos | 55 | 39.7% |
| documents (PDF, Office) | 6 | 18.1% |
| JPEG photos | 124 | 17.2% |
| PNG pictures | 18 | 2.1% |

Any other file can go into the same archive too.

## Download

- **Android:** Google Play - *link added when the app is published*
- **Windows 10 / 11 (64-bit) and Linux (64-bit, glibc 2.34 or newer):** the **Releases** page of this repository. Check each download against
  `SHA256SUMS.txt` on the same page:

```
certutil -hashfile xign-lite.exe SHA256        (Windows)
sha256sum xign-lite                            (Linux)
```

## Use (Windows and Linux)

```
xign-lite a photos.xla  MyFolder  notes.txt     pack a folder and a file into photos.xla
xign-lite l photos.xla                          list what is inside
xign-lite x photos.xla  RestoredHere            restore everything
xign-lite e photos.xla  MyFolder/a.jpg  a.jpg   restore one file
xign-lite t photos.xla                          check every file
```

Options: `-t N` threads, `-o` replace files that already exist when restoring, `--no-open` store documents and
pictures as they are. Exit code: 0 done, 1 error, 2 some items
skipped (named), 3 damaged files named.

## Use (Android)

**Pack files** or **Pack a folder**, then choose where to save the archive. **Open archive** lists what is inside: tap a
file to restore only that file, or use **Restore all**. **Check** reads the whole archive and names any damaged file. The
app asks for no permission: it sees only the files and folders you pick.

## Build from source

```
make            Linux / macOS / WSL (any C11 compiler with pthreads)
make test       the tests: restore exact, one file, 100 damaged archives
build-windows.cmd   Windows (MinGW-w64 GCC)
```

Android: open the `android` folder in Android Studio and build. Every method used is listed with its public source in
[docs/SOURCES.md](docs/SOURCES.md).

## Xign for business

Xign Lite is the free edition. **Xign** for business builds compressors made for a company's own kinds of data - logs,
tables, market data, sensor and scientific data and more - that run on the company's own machines.
Ask for a free sample test on your own files: **xola.compression@gmail.com**

## Licence and names

The code is licensed under the **Apache License, Version 2.0** ([LICENSE](LICENSE), [NOTICE](NOTICE)). It is provided
"AS IS", without warranty. The names "Xign" and "Xign Lite" are trademarks and are **not** covered by the licence
([TRADEMARKS.md](TRADEMARKS.md)). Third-party notices: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).
Privacy: [PRIVACY.md](PRIVACY.md). Contributions: [CONTRIBUTING.md](CONTRIBUTING.md). Security: [SECURITY.md](SECURITY.md).
