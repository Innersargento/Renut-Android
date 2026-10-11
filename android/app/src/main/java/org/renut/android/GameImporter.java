package org.renut.android;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import java.io.*;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Objects;
import java.util.UUID;

/**
 * Imports the game the player picked (a full ISO, or only its default.xex) into the app's
 * private storage and makes it the active game. Runs on the caller's thread and honours its
 * interruption, so the screen that owns it cancels by interrupting that thread.
 *
 * Everything is built in a folder of its own and only registered (active-game.txt) once the
 * executable has been validated, so a failed or cancelled import leaves the previous game alone.
 */
final class GameImporter {
    /** What the screen shows: a message and how far along it is (0..1, or negative when unknown). */
    interface Progress { void update(String message, double fraction) throws IOException; }

    private interface Bytes { void done(long total) throws IOException; }

    private static final long ISO_LIMIT = 16L * 1024 * 1024 * 1024, XEX_LIMIT = 256L * 1024 * 1024;
    // Copying the image is about as long as extracting it, so each takes half of the bar.
    private static final double COPY_SHARE = 0.45;

    private GameImporter() {}

    static void run(Context context, Uri source, boolean wholeDisc, Progress progress) throws IOException {
        File files = context.getFilesDir();
        File stage = new File(files, "import-" + UUID.randomUUID());
        boolean committed = false;
        try {
            if (!stage.mkdir()) throw new IOException(Language.s(R.string.imp_stage_fail));
            File data = new File(stage, "data");
            if (!data.mkdir()) throw new IOException(Language.s(R.string.imp_data_fail));
            File xex;
            if (wholeDisc) {
                File iso = new File(stage, "source.iso");
                long size = sizeOf(context, source);
                progress.update(Language.s(R.string.imp_copy_image), 0);
                copy(context, source, iso, ISO_LIMIT, copied -> progress.update(
                        Language.s(R.string.imp_copy_image_mb, copied / (1024 * 1024)),
                        size > 0 ? Math.min(1.0, copied / (double) size) * COPY_SHARE : -1));
                try (RandomAccessFile input = new RandomAccessFile(iso, "r")) {
                    DiscImage disc = new DiscImage(input);
                    DiscImage.Entry executable = disc.entries().stream()
                            .filter(e -> e.path.equalsIgnoreCase("default.xex")).findFirst()
                            .orElseThrow(() -> new IOException(Language.s(R.string.imp_no_xex)));
                    if (executable.size > XEX_LIMIT) throw new IOException(Language.s(R.string.imp_xex_big));
                    File check = new File(stage, "check.xex");
                    try (OutputStream out = new FileOutputStream(check)) {
                        input.seek(executable.offset);
                        byte[] block = new byte[1024 * 1024];
                        long remaining = executable.size;
                        while (remaining > 0) {
                            int n = (int) Math.min(block.length, remaining);
                            input.readFully(block, 0, n);
                            out.write(block, 0, n);
                            remaining -= n;
                        }
                    }
                    GameFiles.requireEuropean(check);
                    Files.delete(check.toPath());
                    progress.update(Language.s(R.string.imp_extract), COPY_SHARE);
                    disc.extract(data, (done, total) -> progress.update(Language.s(R.string.imp_extract),
                            total > 0 ? COPY_SHARE + (1 - COPY_SHARE) * Math.min(1.0, done / (double) total) : -1));
                }
                Files.delete(iso.toPath());
                xex = Arrays.stream(Objects.requireNonNull(data.listFiles()))
                        .filter(f -> f.getName().equalsIgnoreCase("default.xex")).findFirst()
                        .orElseThrow(() -> new IOException(Language.s(R.string.imp_xex_missing)));
                if (!new File(data, "full-disc.marker").createNewFile()) throw new IOException(Language.s(R.string.imp_marker_fail));
            } else {
                xex = new File(data, "default.xex");
                progress.update(Language.s(R.string.imp_copy_xex), -1);
                copy(context, source, xex, XEX_LIMIT, copied -> progress.update(Language.s(R.string.imp_copy_xex), -1));
            }
            progress.update(Language.s(R.string.imp_verify), 0.99);
            GameFiles.requireEuropean(xex);
            if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException(Language.s(R.string.import_cancelled));
            String previous = activeFolder(files);
            GameFiles.atomicWrite(new File(files, "active-game.txt"), stage.getName() + "/data");
            committed = true;
            discardPrevious(files, previous, stage.getName());
        } finally {
            if (!committed) try { GameFiles.removeTree(stage); } catch (IOException ignored) {}
        }
    }

    /** Name of the folder that holds the active game ("import-…"), or null when there is none. */
    private static String activeFolder(File files) {
        try {
            String value = new String(Files.readAllBytes(new File(files, "active-game.txt").toPath()),
                    java.nio.charset.StandardCharsets.UTF_8).trim();
            int slash = value.indexOf('/');
            return slash > 0 ? value.substring(0, slash) : null;
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * The game that was active before is garbage once the new one is registered, and a full disc
     * is several GB. Only a game folder is touched (import-… with a data folder, no driver files).
     */
    private static void discardPrevious(File files, String previous, String current) {
        if (previous == null || previous.equals(current) || !previous.startsWith("import-")) return;
        File folder = new File(files, previous);
        if (!new File(folder, "data").isDirectory() || new File(folder, "meta.json").exists()) return;
        try { GameFiles.removeTree(folder); } catch (IOException ignored) {}
    }

    private static long sizeOf(Context context, Uri uri) {
        try (Cursor cursor = context.getContentResolver().query(uri, new String[] { OpenableColumns.SIZE }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) return cursor.getLong(0);
        } catch (RuntimeException ignored) {}
        return -1;
    }

    private static void copy(Context context, Uri source, File target, long limit, Bytes progress) throws IOException {
        try (InputStream in = context.getContentResolver().openInputStream(source);
             OutputStream out = new FileOutputStream(target)) {
            if (in == null) throw new IOException(Language.s(R.string.imp_unreadable));
            byte[] buffer = new byte[1024 * 1024];
            long total = 0;
            int count;
            while ((count = in.read(buffer)) != -1) {
                if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException(Language.s(R.string.import_cancelled));
                total += count;
                if (total > limit) throw new IOException(Language.s(R.string.imp_limit));
                if (target.getUsableSpace() < count + 64L * 1024 * 1024) throw new IOException(Language.s(R.string.imp_no_space));
                out.write(buffer, 0, count);
                progress.done(total);
            }
        }
    }
}
