@echo off
rem Xign Lite for Windows: MinGW-w64 GCC, static, stripped, Unicode names. Licensed under the Apache License, Version 2.0.
rem Result: xign-lite.exe here, then a check that it exports no names.
cd /d "%~dp0"
gcc -O2 -std=c11 -municode -D__USE_MINGW_ANSI_STDIO=1 -D_POSIX_C_SOURCE=200809L -DZ_PREFIX -static -s -o xign-lite.exe src\xola.c src\lopen.c src\lite.c src\lite_cli.c src\ljpeg.c src\lcoef.c src\zlib\adler32.c src\zlib\crc32.c src\zlib\deflate.c src\zlib\inffast.c src\zlib\inflate.c src\zlib\inftrees.c src\zlib\trees.c src\zlib\zutil.c -lpthread
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
objdump -p xign-lite.exe | findstr /C:"Export Table" /C:"[Ordinal/Name Pointer]" > nul
if not errorlevel 1 (echo CHECK FAILED: xign-lite.exe has an export table & exit /b 1)
xign-lite.exe version
echo built xign-lite.exe - no export table
