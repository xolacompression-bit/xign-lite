/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
package com.equinumcloud.lite;

/** The native archive core (libxlite.so). Every method returns 0 or a result; a negative number is an error. */
final class Native {
    static { System.loadLibrary("xlite"); }
    private Native() {}

    static final int OK = 0, E_MEM = -1, E_FORMAT = -2, E_DAMAGED = -3, E_ARG = -4, E_IO = -5, E_STOPPED = -6;

    static native long packBegin(int outFd, int level, int threads);           // 0 = failed
    static native int packEntry(long h, byte[] nameUtf8, long mtimeSec, boolean folder);
    static native int packData(long h, byte[] buf, int len);
    static native long[] packEnd(long h);                                       // result, in, out, files, folders, pieces
    static native void packStop(long h);
    static native void packFree(long h);

    static native long arcOpen(int fd, long size, int threads);                 // < 0 = error
    static native int arcCount(long h);
    static native byte[] arcName(long h, int i);
    static native long[] arcInfo(long h, int i);                                // size, mtime, folder
    static native long arcStreamSize(long h);
    static native int arcExtract(long h, int i, int outFd);
    static native int[] arcTest(long h);                                        // result, damaged entries...
    static native void arcClose(long h);
    static native String version();

    static String error(int e) {
        switch (e) {
            case E_MEM: return "not enough memory";
            case E_FORMAT: return "this is not an archive of this app, or its index is damaged";
            case E_DAMAGED: return "damaged";
            case E_ARG: return "a name or setting was not accepted";
            case E_IO: return "the file could not be read or written";
            case E_STOPPED: return "stopped";
            default: return "error " + e;
        }
    }
}
