# Xign Lite - Linux / macOS / WSL:  make   then  make test   (Apache License 2.0)
CC ?= cc
CFLAGS ?= -O2
override CFLAGS += -std=c11 -Wall -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -fvisibility=hidden
ZLIB = src/zlib/adler32.c src/zlib/crc32.c src/zlib/deflate.c src/zlib/inffast.c src/zlib/inflate.c src/zlib/inftrees.c src/zlib/trees.c src/zlib/zutil.c
xign-lite: src/xola.c src/lopen.c src/lite.c src/lite_cli.c src/ljpeg.c src/lcoef.c src/xola.h src/lopen.h src/lite.h src/ljpeg.h src/lcoef.h $(ZLIB)
	$(CC) $(CFLAGS) -DZ_PREFIX -pthread -s -o $@ src/xola.c src/lopen.c src/lite.c src/lite_cli.c src/ljpeg.c src/lcoef.c $(ZLIB)
test: xign-lite
	sh tests/run_tests.sh ./xign-lite
clean:
	rm -f xign-lite xign-lite.exe
.PHONY: test clean
