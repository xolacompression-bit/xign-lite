# The native part of the Lite app: the Xola coder + the Lite archive core + the bridge.
# Licensed under the Apache License, Version 2.0.
cmake_minimum_required(VERSION 3.22.1)
project(xlite C)
set(ENGINE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)
file(GLOB ZLIB_SRC ${ENGINE}/zlib/adler32.c ${ENGINE}/zlib/crc32.c ${ENGINE}/zlib/deflate.c ${ENGINE}/zlib/inffast.c
     ${ENGINE}/zlib/inflate.c ${ENGINE}/zlib/inftrees.c ${ENGINE}/zlib/trees.c ${ENGINE}/zlib/zutil.c)
add_library(xlite SHARED lite_jni.c ${ENGINE}/lite.c ${ENGINE}/xola.c ${ENGINE}/lopen.c ${ENGINE}/ljpeg.c ${ENGINE}/lcoef.c ${ZLIB_SRC})
target_include_directories(xlite PRIVATE ${ENGINE})
target_compile_options(xlite PRIVATE -std=c11 -O2 -fvisibility=hidden -ffunction-sections -fdata-sections
                       -fstack-protector-strong -D_FORTIFY_SOURCE=2 -D_POSIX_C_SOURCE=200809L -DZ_PREFIX -Wall)
# only JNI_OnLoad is visible; no method names in the library; 16 KB pages (Google Play rule for native code)
target_link_options(xlite PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map -Wl,--gc-sections
                    -Wl,-z,max-page-size=16384 -Wl,-z,relro -Wl,-z,now -s)
target_link_libraries(xlite PRIVATE log)
