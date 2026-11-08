/* Copyright 2026 Ximon Paul M. Rodriguez. Licensed under the Apache License, Version 2.0 (see LICENSE and NOTICE). */
package com.equinumcloud.lite;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.content.SharedPreferences;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** One screen: pack files or a folder, open an archive, restore one file or all, check an archive.
 *  Files are reached only through the system file picker; the app asks for no permission. */
public final class MainActivity extends Activity {
    private static final int PICK_FILES = 1, PICK_FOLDER = 2, SAVE_ARCHIVE = 3, OPEN_ARCHIVE = 4, SAVE_ONE = 5, RESTORE_TO = 6;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView status, details;
    private Button cancel;
    private ListView list;
    private RadioGroup strength;
    private final List<Button> actions = new ArrayList<>();

    private final List<Uri> pickedFiles = new ArrayList<>();
    private Uri pickedTree;
    private Jobs.Archive archive;
    private int chosenEntry = -1;
    private volatile boolean busy, stopAsked;
    private final long[] packHandle = {0};

    private static final int TERMS_VERSION = 1;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        SharedPreferences sp = getSharedPreferences("agreement", MODE_PRIVATE);
        if (sp.getInt("version", 0) < TERMS_VERSION) showAgreement(sp); else showMain();
    }

    /** First start: the terms with an UNTICKED box; nothing works until it is ticked. The acceptance (terms version and
     *  time) is kept on the phone only. */
    private void showAgreement(SharedPreferences sp) {
        float dp = getResources().getDisplayMetrics().density;
        int pad = (int) (16 * dp);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        root.setOnApplyWindowInsetsListener((v, in) -> {
            v.setPadding(pad + in.getSystemWindowInsetLeft(), pad + in.getSystemWindowInsetTop(),
                    pad + in.getSystemWindowInsetRight(), pad + in.getSystemWindowInsetBottom());
            return in.consumeSystemWindowInsets();
        });
        TextView t = new TextView(this); t.setText(R.string.agree_title); t.setTextSize(22); t.setTypeface(Typeface.DEFAULT_BOLD);
        TextView body = new TextView(this); body.setText(R.string.agree_text); body.setPadding(0, pad / 2, 0, pad);
        CheckBox box = new CheckBox(this); box.setText(R.string.agree_box); box.setChecked(false);
        Button go = new Button(this); go.setText(R.string.agree_go); go.setEnabled(false);
        box.setOnCheckedChangeListener((v, on) -> go.setEnabled(on));
        go.setOnClickListener(v -> {
            sp.edit().putInt("version", TERMS_VERSION).putLong("accepted_at", System.currentTimeMillis()).apply();
            showMain();
        });
        root.addView(t); root.addView(body); root.addView(box); root.addView(go);
        setContentView(root);
    }

    private void showMain() {
        float dp = getResources().getDisplayMetrics().density;
        int pad = (int) (16 * dp);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        root.setOnApplyWindowInsetsListener((v, in) -> {                  // edge-to-edge (Android 15+): keep clear of the bars
            v.setPadding(pad + in.getSystemWindowInsetLeft(), pad + in.getSystemWindowInsetTop(),
                    pad + in.getSystemWindowInsetRight(), pad + in.getSystemWindowInsetBottom());
            return in.consumeSystemWindowInsets();
        });

        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextSize(24); title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);
        TextView sub = new TextView(this);
        sub.setText(R.string.tagline);
        sub.setPadding(0, 0, 0, pad / 2);
        root.addView(sub);

        strength = new RadioGroup(this);
        strength.setOrientation(RadioGroup.HORIZONTAL);
        RadioButton strong = new RadioButton(this); strong.setText(R.string.strong); strong.setId(View.generateViewId());
        RadioButton fast = new RadioButton(this); fast.setText(R.string.fast); fast.setId(View.generateViewId());
        strength.addView(strong); strength.addView(fast); strength.check(strong.getId());
        root.addView(strength);

        LinearLayout row1 = row(), row2 = row();
        row1.addView(action(R.string.pack_files, v -> pickFiles()));
        row1.addView(action(R.string.pack_folder, v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER)));
        row2.addView(action(R.string.open_archive, v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
            startActivityForResult(i, OPEN_ARCHIVE);
        }));
        row2.addView(action(R.string.restore_all, v -> { if (archive != null) startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), RESTORE_TO); }));
        row2.addView(action(R.string.check, v -> { if (archive != null) runJob(() -> Jobs.check(archive)); }));
        root.addView(row1); root.addView(row2);

        cancel = new Button(this);
        cancel.setText(R.string.stop);
        cancel.setVisibility(View.GONE);
        cancel.setOnClickListener(v -> { stopAsked = true; long h = packHandle[0]; if (h != 0) Native.packStop(h); });
        root.addView(cancel);

        status = new TextView(this);
        status.setTextSize(16); status.setPadding(0, pad / 2, 0, pad / 4);
        status.setText(R.string.hello);
        root.addView(status);
        details = new TextView(this);
        details.setTextIsSelectable(true);
        root.addView(details);

        list = new ListView(this);
        list.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        list.setOnItemClickListener((p, v, pos, id) -> {
            if (archive == null || busy || archive.folder[pos]) return;
            chosenEntry = pos;
            String name = archive.names[pos];
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                    .setType("application/octet-stream").putExtra(Intent.EXTRA_TITLE, name.substring(name.lastIndexOf('/') + 1));
            startActivityForResult(i, SAVE_ONE);
        });
        root.addView(list);

        TextView foot = new TextView(this);
        foot.setText(getString(R.string.footer, Native.version()));
        foot.setTextSize(12); foot.setGravity(Gravity.CENTER);
        root.addView(foot);
        Button about = new Button(this);
        about.setText(R.string.about); about.setAllCaps(false);
        about.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle(R.string.about)
                .setMessage(getString(R.string.about_text, Native.version())).setPositiveButton(android.R.string.ok, null).show());
        root.addView(about);
        setContentView(root);
    }

    private LinearLayout row() {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        return r;
    }

    private Button action(int text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text); b.setAllCaps(false);
        b.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        b.setOnClickListener(v -> { if (!busy) l.onClick(v); });
        actions.add(b);
        return b;
    }

    private void pickFiles() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, PICK_FILES);
    }

    private void askWhereToSave(String suggested) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                .setType("application/octet-stream").putExtra(Intent.EXTRA_TITLE, suggested + ".xla");
        startActivityForResult(i, SAVE_ARCHIVE);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null) return;
        switch (req) {
            case PICK_FILES: {
                pickedFiles.clear(); pickedTree = null;
                ClipData clip = data.getClipData();
                if (clip != null) for (int k = 0; k < clip.getItemCount(); k++) pickedFiles.add(clip.getItemAt(k).getUri());
                else if (data.getData() != null) pickedFiles.add(data.getData());
                if (pickedFiles.isEmpty()) return;
                String first = Jobs.displayName(getContentResolver(), pickedFiles.get(0));
                int dot = first.lastIndexOf('.');
                askWhereToSave(pickedFiles.size() == 1 && dot > 0 ? first.substring(0, dot) : "archive");
                break;
            }
            case PICK_FOLDER: {
                pickedFiles.clear(); pickedTree = data.getData();
                Uri doc = android.provider.DocumentsContract.buildDocumentUriUsingTree(pickedTree,
                        android.provider.DocumentsContract.getTreeDocumentId(pickedTree));
                askWhereToSave(Jobs.cleanPart(Jobs.displayName(getContentResolver(), doc)));
                break;
            }
            case SAVE_ARCHIVE: {
                final Uri out = data.getData();
                final List<Uri> files = new ArrayList<>(pickedFiles);
                final Uri tree = pickedTree;
                final int level = strength.getCheckedRadioButtonId() == strength.getChildAt(1).getId() ? 3 : 6;
                runJob(() -> Jobs.pack(getContentResolver(), files, tree, out, level, progress(), packHandle));
                break;
            }
            case OPEN_ARCHIVE: openArchive(data.getData()); break;
            case SAVE_ONE: {
                final Uri out = data.getData(); final int i = chosenEntry;
                if (archive != null && i >= 0) runJob(() -> Jobs.restoreOne(getContentResolver(), archive, i, out));
                break;
            }
            case RESTORE_TO: {
                final Uri tree = data.getData();
                if (archive != null) runJob(() -> Jobs.restoreAll(getContentResolver(), archive, tree, progress()));
                break;
            }
        }
    }

    private void openArchive(Uri u) {
        if (archive != null) { archive.close(); archive = null; }
        String[] err = {null};
        archive = Jobs.open(getContentResolver(), u, err);
        if (archive == null) { status.setText(err[0]); details.setText(""); list.setAdapter(null); return; }
        List<String> rows = new ArrayList<>();
        long total = 0;
        for (int i = 0; i < archive.names.length; i++) {
            if (archive.folder[i]) rows.add(archive.names[i] + "/");
            else { rows.add(archive.names[i] + "   " + human(archive.sizes[i])); total += archive.sizes[i]; }
        }
        list.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, rows));
        status.setText(getString(R.string.opened, archive.names.length, human(total)));
        details.setText(R.string.tap_to_restore);
    }

    private static String human(long n) {
        if (n < 1000) return n + " B";
        if (n < 1_000_000) return String.format(Locale.US, "%.1f KB", n / 1e3);
        if (n < 1_000_000_000) return String.format(Locale.US, "%.1f MB", n / 1e6);
        return String.format(Locale.US, "%.2f GB", n / 1e9);
    }

    private Jobs.Progress progress() {
        return new Jobs.Progress() {
            long last;
            @Override public void step(String text) {
                long now = System.currentTimeMillis();
                if (now - last < 150) return;
                last = now;
                ui.post(() -> status.setText(text));
            }
            @Override public boolean cancelled() { return stopAsked; }
        };
    }

    private interface Work { Jobs.Result run(); }

    private void runJob(Work w) {
        if (busy) return;
        busy = true; stopAsked = false;
        for (Button b : actions) b.setEnabled(false);
        cancel.setVisibility(View.VISIBLE);
        status.setText(R.string.working); details.setText("");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);   // keep the work going while the screen is on
        new Thread(() -> {
            Jobs.Result r;
            try { r = w.run(); }
            catch (Throwable t) { r = new Jobs.Result(); r.text = "Stopped by an error: " + t.getClass().getSimpleName(); }
            final Jobs.Result fr = r;
            ui.post(() -> done(fr));
        }, "work").start();
    }

    private void done(Jobs.Result r) {
        busy = false;
        for (Button b : actions) b.setEnabled(true);
        cancel.setVisibility(View.GONE);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        status.setText(r.text);
        if (!r.damaged.isEmpty()) {
            StringBuilder s = new StringBuilder(getString(R.string.named));
            for (String n : r.damaged) s.append("\n- ").append(n);
            details.setText(s);
            if (!r.ok) new AlertDialog.Builder(this).setTitle(R.string.attention).setMessage(s).setPositiveButton(android.R.string.ok, null).show();
        }
    }

    @Override protected void onDestroy() {
        if (archive != null && !busy) archive.close();
        super.onDestroy();
    }
}
