/* The bridge between the Android app and the Lite archive core.
 * Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE).
 * Only JNI_OnLoad is exported (version script); every other name stays inside the library. */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "lite.h"

typedef struct { int fd; } fdw;

static int fd_write(void *c, const uint8_t *p, size_t n) {
    int fd = ((fdw *)c)->fd;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int64_t fd_pread(void *c, uint8_t *p, size_t n, uint64_t off) {
    for (;;) {
        ssize_t k = pread(((fdw *)c)->fd, p, n, (off_t)off);
        if (k < 0 && errno == EINTR) continue;
        return k;
    }
}

typedef struct { lite_packer *pk; fdw w; } pack_h;
typedef struct { lite_reader *r; fdw in; } read_h;

static jlong packBegin(JNIEnv *env, jclass c, jint fd, jint level, jint threads) {
    (void)env; (void)c;
    pack_h *h = calloc(1, sizeof *h);
    if (!h) return 0;
    h->w.fd = fd;
    lite_opts o; lite_default_opts(&o);
    o.level = level; o.threads = threads;
    int err = 0;
    h->pk = lite_pack_begin(fd_write, &h->w, &o, &err);
    if (!h->pk) { free(h); return 0; }
    return (jlong)(intptr_t)h;
}
static jint packEntry(JNIEnv *env, jclass c, jlong hh, jbyteArray name, jlong mtime, jboolean dir) {   /* name: real UTF-8 bytes */
    (void)c;
    pack_h *h = (pack_h *)(intptr_t)hh;
    jsize n = (*env)->GetArrayLength(env, name);
    char *s = malloc((size_t)n + 1);
    if (!s) return LITE_E_MEM;
    (*env)->GetByteArrayRegion(env, name, 0, n, (jbyte *)s);
    s[n] = 0;
    int rc = strlen(s) == (size_t)n ? lite_pack_entry(h->pk, s, (int64_t)mtime, dir ? 1 : 0) : LITE_E_ARG;
    free(s);
    return rc;
}
static jint packData(JNIEnv *env, jclass c, jlong hh, jbyteArray buf, jint len) {
    (void)c;
    pack_h *h = (pack_h *)(intptr_t)hh;
    jbyte *p = (*env)->GetPrimitiveArrayCritical(env, buf, NULL);
    if (!p) return LITE_E_MEM;
    /* copy out first: the core may take a long time, and a critical section must stay short */
    uint8_t *tmp = malloc((size_t)len);
    if (tmp) memcpy(tmp, p, (size_t)len);
    (*env)->ReleasePrimitiveArrayCritical(env, buf, p, JNI_ABORT);
    if (!tmp) return LITE_E_MEM;
    int rc = lite_pack_data(h->pk, tmp, (size_t)len);
    free(tmp);
    return rc;
}
static jlongArray packEnd(JNIEnv *env, jclass c, jlong hh) {
    (void)c;
    pack_h *h = (pack_h *)(intptr_t)hh;
    lite_stats st = {0};
    int rc = lite_pack_end(h->pk, &st);
    jlong v[6] = {rc, (jlong)st.in_bytes, (jlong)st.out_bytes, st.files, st.folders, st.pieces};
    jlongArray a = (*env)->NewLongArray(env, 6);
    if (a) (*env)->SetLongArrayRegion(env, a, 0, 6, v);
    return a;
}
static void packStop(JNIEnv *env, jclass c, jlong hh) { (void)env; (void)c; *lite_pack_stop_flag(((pack_h *)(intptr_t)hh)->pk) = 1; }
static void packFree(JNIEnv *env, jclass c, jlong hh) {
    (void)env; (void)c;
    pack_h *h = (pack_h *)(intptr_t)hh;
    if (h) { lite_pack_free(h->pk); free(h); }
}

static jlong arcOpen(JNIEnv *env, jclass c, jint fd, jlong size, jint threads) {
    (void)env; (void)c;
    read_h *h = calloc(1, sizeof *h);
    if (!h) return LITE_E_MEM;
    h->in.fd = fd;
    int err = 0;
    h->r = lite_open(fd_pread, &h->in, (uint64_t)size, threads, &err);
    if (!h->r) { free(h); return err ? err : LITE_E_FORMAT; }
    return (jlong)(intptr_t)h;
}
static jint arcCount(JNIEnv *env, jclass c, jlong hh) { (void)env; (void)c; return (jint)lite_count(((read_h *)(intptr_t)hh)->r); }
static jbyteArray arcName(JNIEnv *env, jclass c, jlong hh, jint i) {   /* real UTF-8 bytes */
    (void)c;
    const lite_entry *e = lite_get(((read_h *)(intptr_t)hh)->r, (uint32_t)i);
    if (!e) return NULL;
    jsize n = (jsize)strlen(e->name);
    jbyteArray a = (*env)->NewByteArray(env, n);
    if (a) (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)e->name);
    return a;
}
static jlongArray arcInfo(JNIEnv *env, jclass c, jlong hh, jint i) {   /* size, mtime, is_dir */
    (void)c;
    const lite_entry *e = lite_get(((read_h *)(intptr_t)hh)->r, (uint32_t)i);
    if (!e) return NULL;
    jlong v[3] = {(jlong)e->size, (jlong)e->mtime, e->is_dir};
    jlongArray a = (*env)->NewLongArray(env, 3);
    if (a) (*env)->SetLongArrayRegion(env, a, 0, 3, v);
    return a;
}
static jlong arcStreamSize(JNIEnv *env, jclass c, jlong hh) { (void)env; (void)c; return (jlong)lite_stream_size(((read_h *)(intptr_t)hh)->r); }
static jint arcExtract(JNIEnv *env, jclass c, jlong hh, jint i, jint outFd) {
    (void)env; (void)c;
    fdw w = {outFd};
    return lite_extract(((read_h *)(intptr_t)hh)->r, (uint32_t)i, fd_write, &w);
}
typedef struct { int *v; int n, cap; } badlist;
static void on_bad(void *c, uint32_t i) {
    badlist *b = c;
    if (b->n == b->cap) { int nc = b->cap ? b->cap * 2 : 16; int *q = realloc(b->v, (size_t)nc * sizeof *q); if (!q) return; b->v = q; b->cap = nc; }
    b->v[b->n++] = (int)i;
}
static jintArray arcTest(JNIEnv *env, jclass c, jlong hh) {   /* [result, damaged entry numbers...] */
    (void)c;
    badlist b = {0};
    int rc = lite_test(((read_h *)(intptr_t)hh)->r, on_bad, &b);
    jintArray a = (*env)->NewIntArray(env, 1 + b.n);
    if (a) { jint r0 = rc; (*env)->SetIntArrayRegion(env, a, 0, 1, &r0); if (b.n) (*env)->SetIntArrayRegion(env, a, 1, b.n, b.v); }
    free(b.v);
    return a;
}
static void arcClose(JNIEnv *env, jclass c, jlong hh) {
    (void)env; (void)c;
    read_h *h = (read_h *)(intptr_t)hh;
    if (h) { lite_close(h->r); free(h); }
}
static jstring version(JNIEnv *env, jclass c) { (void)c; return (*env)->NewStringUTF(env, LITE_VERSION_STRING); }

static const JNINativeMethod M[] = {
    {"packBegin", "(III)J", (void *)packBegin},
    {"packEntry", "(J[BJZ)I", (void *)packEntry},
    {"packData", "(J[BI)I", (void *)packData},
    {"packEnd", "(J)[J", (void *)packEnd},
    {"packStop", "(J)V", (void *)packStop},
    {"packFree", "(J)V", (void *)packFree},
    {"arcOpen", "(IJI)J", (void *)arcOpen},
    {"arcCount", "(J)I", (void *)arcCount},
    {"arcName", "(JI)[B", (void *)arcName},
    {"arcInfo", "(JI)[J", (void *)arcInfo},
    {"arcStreamSize", "(J)J", (void *)arcStreamSize},
    {"arcExtract", "(JII)I", (void *)arcExtract},
    {"arcTest", "(J)[I", (void *)arcTest},
    {"arcClose", "(J)V", (void *)arcClose},
    {"version", "()Ljava/lang/String;", (void *)version},
};

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass k = (*env)->FindClass(env, "com/equinumcloud/lite/Native");
    if (!k) return JNI_ERR;
    if ((*env)->RegisterNatives(env, k, M, (jint)(sizeof M / sizeof M[0])) != 0) return JNI_ERR;
    return JNI_VERSION_1_6;
}
