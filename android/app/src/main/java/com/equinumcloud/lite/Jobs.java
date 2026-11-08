/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
package com.equinumcloud.lite;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** The work the app does. Everything here runs on a background thread; it only reads and writes the files the
 *  user picked with the system file picker. Nothing is ever sent anywhere (the app has no internet permission). */
final class Jobs {
    interface Progress { void step(String text); boolean cancelled(); }

    static final class Result {
        boolean ok; String text = ""; final List<String> damaged = new ArrayList<>();
    }

    static int threads() { return Math.max(1, Math.min(8, Runtime.getRuntime().availableProcessors())); }

    /** a name the archive accepts: no '/', '\\', ':', control characters, "." or ".." */
    static String cleanPart(String s) {
        if (s == null || s.isEmpty()) return "_";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            b.append(c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' ? '_' : c);
        }
        String t = b.toString();
        return t.equals(".") || t.equals("..") ? "_" : t;
    }

    static String mb(long n) { return String.format(java.util.Locale.US, "%.2f MB", n / 1e6); }

    // ------------------------------------------------------------------ pack
    private static final class Item { Uri uri; String name; boolean folder; long mtime; }

    private static void walkTree(ContentResolver cr, Uri tree, String docId, String prefix, List<Item> out, int depth) {
        if (depth > 64) return;
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_LAST_MODIFIED};
        List<String[]> rows = new ArrayList<>();
        try (Cursor c = cr.query(kids, cols, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) rows.add(new String[]{c.getString(0), c.getString(1), c.getString(2), c.isNull(3) ? "0" : c.getString(3)});
        } catch (RuntimeException e) { return; }
        rows.sort((a, b) -> a[1] == null ? -1 : b[1] == null ? 1 : a[1].compareTo(b[1]));
        for (String[] r : rows) {
            Item it = new Item();
            it.name = prefix + "/" + cleanPart(r[1]);
            it.folder = DocumentsContract.Document.MIME_TYPE_DIR.equals(r[2]);
            try { it.mtime = Long.parseLong(r[3]) / 1000; } catch (NumberFormatException e) { it.mtime = 0; }
            it.uri = DocumentsContract.buildDocumentUriUsingTree(tree, r[0]);
            out.add(it);
            if (it.folder) walkTree(cr, tree, r[0], it.name, out, depth + 1);
        }
    }

    private static boolean sameDoc(Uri a, Uri b) {
        try { return a.getAuthority() != null && a.getAuthority().equals(b.getAuthority())
                && DocumentsContract.getDocumentId(a).equals(DocumentsContract.getDocumentId(b)); }
        catch (RuntimeException e) { return a.equals(b); }
    }

    static String displayName(ContentResolver cr, Uri u) {
        try (Cursor c = cr.query(u, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (RuntimeException ignored) { }
        String s = u.getLastPathSegment();
        return s == null ? "file" : s.substring(s.lastIndexOf('/') + 1);
    }

    /** files: picked documents (tree == null), or every document under a picked folder (tree != null) */
    static Result pack(ContentResolver cr, List<Uri> files, Uri tree, Uri out, int level, Progress pg, long[] handleOut) {
        Result res = new Result();
        List<Item> items = new ArrayList<>();
        if (tree != null) {
            String root = DocumentsContract.getTreeDocumentId(tree);
            Uri rootDoc = DocumentsContract.buildDocumentUriUsingTree(tree, root);
            Item top = new Item(); top.folder = true; top.uri = rootDoc; top.name = cleanPart(displayName(cr, rootDoc));
            items.add(top);
            walkTree(cr, tree, root, top.name, items, 0);
        } else {
            Map<String, Integer> seen = new HashMap<>();
            for (Uri u : files) {
                Item it = new Item(); it.uri = u; it.name = cleanPart(displayName(cr, u));
                Integer k = seen.get(it.name);                       // two picked files with one name: keep both
                seen.put(it.name, k == null ? 1 : k + 1);
                if (k != null) it.name = "(" + (k + 1) + ") " + it.name;
                items.add(it);
            }
        }
        long t0 = System.nanoTime();
        try (ParcelFileDescriptor po = cr.openFileDescriptor(out, "w")) {
            if (po == null) { res.text = "The archive could not be created."; return res; }
            long h = Native.packBegin(po.getFd(), level, threads());
            if (h == 0) { res.text = "Not enough memory to start."; return res; }
            handleOut[0] = h;
            byte[] buf = new byte[1 << 20];
            int rc = Native.OK, n = 0, skipped = 0;
            try {
                for (Item it : items) {
                    if (pg.cancelled()) { rc = Native.E_STOPPED; break; }
                    pg.step("Packing " + (++n) + " of " + items.size() + ": " + it.name);
                    if (sameDoc(it.uri, out)) continue;                     // never the archive itself
                    if (it.folder) {
                        rc = Native.packEntry(h, it.name.getBytes(StandardCharsets.UTF_8), it.mtime, true);
                        if (rc == Native.E_ARG) { rc = 0; skipped++; res.damaged.add("name not accepted: " + it.name); continue; }
                        if (rc != 0) break;
                        continue;
                    }
                    InputStream in;
                    try { in = cr.openInputStream(it.uri); } catch (IOException | SecurityException e) { in = null; }
                    if (in == null) { skipped++; res.damaged.add("could not be read: " + it.name); continue; }
                    try (InputStream is = in) {
                        rc = Native.packEntry(h, it.name.getBytes(StandardCharsets.UTF_8), it.mtime, false);
                        if (rc == Native.E_ARG) { skipped++; res.damaged.add("name not accepted: " + it.name); rc = 0; continue; }
                        int k;
                        while (rc == 0 && (k = is.read(buf)) > 0) rc = Native.packData(h, buf, k);
                    } catch (IOException e) { rc = Native.E_IO; }
                    if (rc != 0) break;
                }
                long[] st = rc == 0 ? Native.packEnd(h) : null;
                if (st != null) rc = (int) st[0];
                double sec = (System.nanoTime() - t0) / 1e9;
                if (rc == 0 && st != null) {
                    res.ok = true;
                    double pct = st[1] > 0 ? 100.0 - 100.0 * st[2] / st[1] : 0;
                    res.text = String.format(java.util.Locale.US, "%d files, %d folders\n%s -> %s  (%.1f%% smaller)\n%.1f MB/s%s",
                            st[3], st[4], mb(st[1]), mb(st[2]), pct, sec > 0 ? st[1] / 1e6 / sec : 0,
                            skipped > 0 ? "\n" + skipped + " file(s) could not be read (listed below)" : "");
                }
            } finally {
                handleOut[0] = 0;
                Native.packFree(h);
            }
            if (rc != 0) res.text = "Not finished: " + Native.error(rc) + ".";
        } catch (IOException | SecurityException e) {
            res.text = "The archive could not be written.";
        }
        if (!res.ok) { try { DocumentsContract.deleteDocument(cr, out); } catch (Exception ignored) { } }  // never a half archive
        return res;
    }

    // ------------------------------------------------------------------ open, restore, check
    static final class Archive implements AutoCloseable {
        final ParcelFileDescriptor pfd; final long h; final String[] names; final long[] sizes; final boolean[] folder;
        Archive(ParcelFileDescriptor p, long hh) {
            pfd = p; h = hh;
            int n = Native.arcCount(h);
            names = new String[n]; sizes = new long[n]; folder = new boolean[n];
            for (int i = 0; i < n; i++) {
                names[i] = new String(Native.arcName(h, i), StandardCharsets.UTF_8);
                long[] in = Native.arcInfo(h, i);
                sizes[i] = in[0]; folder[i] = in[2] != 0;
            }
        }
        @Override public void close() { Native.arcClose(h); try { pfd.close(); } catch (IOException ignored) { } }
    }

    static Archive open(ContentResolver cr, Uri u, String[] err) {
        try {
            ParcelFileDescriptor p = cr.openFileDescriptor(u, "r");
            if (p == null) { err[0] = "The file could not be opened."; return null; }
            long h = Native.arcOpen(p.getFd(), p.getStatSize(), threads());
            if (h < 0) { err[0] = "Not opened: " + Native.error((int) h) + "."; p.close(); return null; }
            return new Archive(p, h);
        } catch (IOException | SecurityException e) {
            err[0] = "The file could not be opened.";
            return null;
        }
    }

    /** restore one entry into a document the user created */
    static Result restoreOne(ContentResolver cr, Archive a, int i, Uri out) {
        Result r = new Result();
        int rc;
        try (ParcelFileDescriptor po = cr.openFileDescriptor(out, "w")) {
            rc = po == null ? Native.E_IO : Native.arcExtract(a.h, i, po.getFd());
        } catch (IOException | SecurityException e) { rc = Native.E_IO; }
        if (rc == 0) { r.ok = true; r.text = "Restored exactly: " + a.names[i]; }
        else {
            try { DocumentsContract.deleteDocument(cr, out); } catch (Exception ignored) { }
            if (rc == Native.E_DAMAGED) { r.damaged.add(a.names[i]); r.text = "This file is damaged in the archive and was not written."; }
            else r.text = "Not restored: " + Native.error(rc) + ".";
        }
        return r;
    }

    /** restore every entry under a folder the user picked */
    static Result restoreAll(ContentResolver cr, Archive a, Uri tree, Progress pg) {
        Result r = new Result();
        Map<String, Uri> dirs = new HashMap<>();
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        dirs.put("", DocumentsContract.buildDocumentUriUsingTree(tree, rootId));
        long t0 = System.nanoTime(); int good = 0, fails = 0;
        for (int i = 0; i < a.names.length; i++) {
            if (pg.cancelled()) { r.text = "Stopped after " + good + " files."; return r; }
            pg.step("Restoring " + (i + 1) + " of " + a.names.length + ": " + a.names[i]);
            String name = a.names[i];
            int cut = name.lastIndexOf('/');
            String parent = cut < 0 ? "" : name.substring(0, cut), leaf = name.substring(cut + 1);
            try {
                Uri pu = folderFor(cr, dirs, parent);
                if (pu == null) { fails++; continue; }
                if (a.folder[i]) { if (!dirs.containsKey(name)) { Uri d = DocumentsContract.createDocument(cr, pu, DocumentsContract.Document.MIME_TYPE_DIR, leaf); if (d != null) dirs.put(name, d); } continue; }
                Uri f = DocumentsContract.createDocument(cr, pu, "application/octet-stream", leaf);
                if (f == null) { fails++; continue; }
                int rc;
                try (ParcelFileDescriptor po = cr.openFileDescriptor(f, "w")) { rc = po == null ? Native.E_IO : Native.arcExtract(a.h, i, po.getFd()); }
                if (rc == 0) good++;
                else {
                    DocumentsContract.deleteDocument(cr, f);
                    if (rc == Native.E_DAMAGED) r.damaged.add(name); else fails++;
                }
            } catch (IOException | RuntimeException e) { fails++; }
        }
        double sec = (System.nanoTime() - t0) / 1e9;
        r.ok = fails == 0 && r.damaged.isEmpty();
        r.text = String.format(java.util.Locale.US, "%d files restored exactly  (%.1f MB/s)%s%s", good,
                sec > 0 ? Native.arcStreamSize(a.h) / 1e6 / sec : 0,
                r.damaged.isEmpty() ? "" : "\n" + r.damaged.size() + " damaged file(s) were not written (listed below)",
                fails == 0 ? "" : "\n" + fails + " item(s) could not be written");
        return r;
    }

    private static Uri folderFor(ContentResolver cr, Map<String, Uri> dirs, String path) throws IOException {
        Uri u = dirs.get(path);
        if (u != null) return u;
        int cut = path.lastIndexOf('/');
        Uri parent = folderFor(cr, dirs, cut < 0 ? "" : path.substring(0, cut));
        if (parent == null) return null;
        try {
            u = DocumentsContract.createDocument(cr, parent, DocumentsContract.Document.MIME_TYPE_DIR, path.substring(cut + 1));
        } catch (IOException | RuntimeException e) { u = null; }
        if (u != null) dirs.put(path, u);
        return u;
    }

    static Result check(Archive a) {
        Result r = new Result();
        long t0 = System.nanoTime();
        int[] t = Native.arcTest(a.h);
        double sec = (System.nanoTime() - t0) / 1e9;
        if (t[0] < 0) { r.text = "Not checked: " + Native.error(t[0]) + "."; return r; }
        for (int k = 1; k < t.length; k++) r.damaged.add(a.names[t[k]]);
        r.ok = t[0] == 0;
        r.text = r.ok ? String.format(java.util.Locale.US, "All %d entries are good - every file checked  (%.1f MB/s)", a.names.length,
                sec > 0 ? Native.arcStreamSize(a.h) / 1e6 / sec : 0)
                : t[0] + " of " + a.names.length + " entries are damaged (listed below); the others are good.";
        return r;
    }
}
