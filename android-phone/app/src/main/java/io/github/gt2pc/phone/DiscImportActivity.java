package io.github.gt2pc.phone;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

// The launcher of the phone build. With a disc installed it starts the game at once; without one (or from the
// "Import disc" shortcut / the game's "Import another game disc...") it lets the player pick their own 2352-byte BIN
// with the system file picker, copies it into the app's folder and installs it (disc_import.cpp). Game data is never
// bundled or downloaded.
public final class DiscImportActivity extends Activity {
    public static final String ACTION_IMPORT = "io.github.gt2pc.phone.IMPORT_DISC";
    private static final int REQUEST_PICK_DISC = 1;
    private static final int REQUEST_SAVE_LOG = 2;
    private static final int RAW_SECTOR = 2352;
    private static final String[] DISC_FOLDERS = {"arcade", "simulation", "combined"};

    private static native String nativeInstall(String bin, String root);

    private TextView status;
    private ProgressBar progress;
    private Button pickButton, playButton, logButton;
    private volatile boolean busy;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        final boolean importRequested = ACTION_IMPORT.equals(getIntent().getAction());
        if (state == null && !importRequested && hasAnyDisc()) {
            startGame();
            return;
        }
        buildUi();
        refresh(null);
    }

    private File dataRoot() {
        File root = getExternalFilesDir(null);
        if (root == null) throw new IllegalStateException("No app storage available");
        return root;
    }

    private boolean hasAnyDisc() {
        File root = getExternalFilesDir(null);
        if (root == null) return false;
        for (String folder : DISC_FOLDERS) if (new File(root, folder + "/disc.raw2352").isFile()) return true;
        return false;
    }

    private String installedList() {
        StringBuilder out = new StringBuilder();
        File root = getExternalFilesDir(null);
        if (root == null) return "";
        if (new File(root, "arcade/disc.raw2352").isFile()) out.append(getString(R.string.disc_arcade)).append('\n');
        if (new File(root, "simulation/disc.raw2352").isFile()) out.append(getString(R.string.disc_simulation)).append('\n');
        if (new File(root, "combined/disc.raw2352").isFile()) out.append(getString(R.string.disc_combined)).append('\n');
        return out.toString().trim();
    }

    private void startGame() {
        startActivity(new Intent(this, PhoneGameActivity.class));
        finish();
    }

    private void buildUi() {
        final float dp = getResources().getDisplayMetrics().density;
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_HORIZONTAL);
        final int pad = Math.round(24 * dp);
        column.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(R.string.import_title);
        title.setTextSize(24);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(title);

        TextView help = new TextView(this);
        help.setText(R.string.import_help);
        help.setTextSize(15);
        help.setPadding(0, Math.round(12 * dp), 0, Math.round(12 * dp));
        column.addView(help);

        status = new TextView(this);
        status.setTextSize(15);
        status.setPadding(0, 0, 0, Math.round(12 * dp));
        column.addView(status);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        column.addView(progress, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        pickButton = new Button(this);
        pickButton.setText(R.string.import_pick);
        pickButton.setOnClickListener(v -> pickDisc());
        column.addView(pickButton, fullWidth(dp));

        playButton = new Button(this);
        playButton.setText(R.string.import_play);
        playButton.setOnClickListener(v -> startGame());
        column.addView(playButton, fullWidth(dp));

        logButton = new Button(this);
        logButton.setText(R.string.import_save_log);
        logButton.setOnClickListener(v -> saveLog());
        column.addView(logButton, fullWidth(dp));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(column);
        setContentView(scroll);
    }

    private static LinearLayout.LayoutParams fullWidth(float dp) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = Math.round(8 * dp);
        return params;
    }

    // `message` null: show what is installed.
    private void refresh(String message) {
        final String installed = installedList();
        String text = message != null ? message
            : installed.isEmpty() ? getString(R.string.import_none) : getString(R.string.import_installed, installed);
        status.setText(text);
        pickButton.setEnabled(!busy);
        playButton.setEnabled(!busy && hasAnyDisc());
        playButton.setVisibility(hasAnyDisc() ? View.VISIBLE : View.GONE);
        logButton.setEnabled(!busy);
        progress.setVisibility(busy ? View.VISIBLE : View.GONE);
    }

    private void pickDisc() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        startActivityForResult(pick, REQUEST_PICK_DISC);
    }

    private void saveLog() {
        Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        save.addCategory(Intent.CATEGORY_OPENABLE);
        save.setType("text/plain");
        save.putExtra(Intent.EXTRA_TITLE, "gt2-phone-log.txt");
        startActivityForResult(save, REQUEST_SAVE_LOG);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        if (request == REQUEST_PICK_DISC) startImport(data.getData());
        if (request == REQUEST_SAVE_LOG) writeLog(data.getData());
    }

    private void writeLog(Uri target) {
        try (OutputStream out = getContentResolver().openOutputStream(target)) {
            if (out == null) throw new IOException("cannot open the chosen file");
            for (String name : new String[] {"gt2game.log", "gt2game-error.log"}) {
                File file = new File(getFilesDir(), name);
                out.write(("===== " + name + " =====\n").getBytes(StandardCharsets.UTF_8));
                if (!file.isFile()) { out.write("(none)\n".getBytes(StandardCharsets.UTF_8)); continue; }
                try (InputStream in = new FileInputStream(file)) { copy(in, out, null, 0); }
                out.write('\n');
            }
            refresh(getString(R.string.import_log_saved));
        } catch (IOException e) {
            refresh(getString(R.string.import_failed, String.valueOf(e.getMessage())));
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (RuntimeException ignored) { }
        return "";
    }

    private long displaySize(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (RuntimeException ignored) { }
        return -1;
    }

    private void startImport(final Uri uri) {
        final String name = displayName(uri).toLowerCase(java.util.Locale.ROOT);
        final long size = displaySize(uri);
        if (name.endsWith(".cue")) { refresh(getString(R.string.import_cue)); return; }
        if (name.endsWith(".iso") || (size > 0 && size % RAW_SECTOR != 0)) { refresh(getString(R.string.import_not_raw)); return; }
        if (name.endsWith(".chd") || name.endsWith(".pbp") || name.endsWith(".ecm") || name.endsWith(".zip") || name.endsWith(".7z")) {
            refresh(getString(R.string.import_compressed));
            return;
        }
        final File root = dataRoot();
        // The image is copied once and then moved into place; the extracted archive needs about as much again.
        if (size > 0 && root.getUsableSpace() < size * 2 + (64L << 20)) {
            refresh(getString(R.string.import_space, (size * 2) >> 20));
            return;
        }
        busy = true;
        progress.setProgress(0);
        refresh(getString(R.string.import_copying));
        new Thread(() -> {
            File staging = new File(root, ".import");
            File bin = new File(staging, "disc.bin");
            String message;
            try {
                if (!staging.isDirectory() && !staging.mkdirs()) throw new IOException("cannot create " + staging);
                try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(bin)) {
                    if (in == null) throw new IOException("cannot open the chosen file");
                    copy(in, out, this::postProgress, size);
                }
                runOnUiThread(() -> refresh(getString(R.string.import_installing)));
                System.loadLibrary("gt2game");
                final String result = nativeInstall(bin.getAbsolutePath(), root.getAbsolutePath());
                if (result.startsWith("OK:")) {
                    final String mode = result.substring(3);
                    message = getString(R.string.import_done, getString(
                        mode.equals("arcade") ? R.string.disc_arcade : mode.equals("simulation") ? R.string.disc_simulation : R.string.disc_combined));
                } else {
                    message = getString(R.string.import_failed, result.startsWith("ERR:") ? result.substring(4) : result);
                }
            } catch (IOException | RuntimeException | UnsatisfiedLinkError e) {
                message = getString(R.string.import_failed, String.valueOf(e.getMessage()));
            }
            if (bin.isFile() && !bin.delete()) bin.deleteOnExit();
            final String shown = message;
            runOnUiThread(() -> { busy = false; refresh(shown); });
        }, "gt2-disc-import").start();
    }

    private interface Progress { void update(long done, long total); }

    private void postProgress(long done, long total) {
        if (total <= 0) return;
        final int value = (int) Math.min(1000, done * 1000 / total);
        runOnUiThread(() -> progress.setProgress(value));
    }

    private static void copy(InputStream in, OutputStream out, Progress progress, long total) throws IOException {
        byte[] buffer = new byte[1 << 20];
        long done = 0, reported = 0;
        for (int n; (n = in.read(buffer)) > 0; ) {
            out.write(buffer, 0, n);
            done += n;
            if (progress != null && done - reported >= (8 << 20)) { progress.update(done, total); reported = done; }
        }
        if (progress != null) progress.update(done, total);
    }
}
