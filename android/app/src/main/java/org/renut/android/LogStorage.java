package org.renut.android;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Where the files the player reads live: Documents/renut (renut.toml, the runtime log, logcat captures). Reaching
 * Documents needs the "all files" access of Android 11 and later, which the player grants in the system settings;
 * without it the configuration stays in the app's own files and the logs go to the app's own Documents folder
 * (Android/data/.../files/Documents/renut), which needs nothing.
 */
final class LogStorage {
    static final String FOLDER = "renut";
    static final String CONFIG = "renut.toml";
    /** One line, read by the runtime before it starts logging: the file the log goes to (absent: the default). */
    static final String LOG_TARGET = "log-target.txt";

    private LogStorage() {}

    /** Documents/renut on the shared storage. */
    @SuppressWarnings("deprecation")
    static File documents() {
        return new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS), FOLDER);
    }

    static boolean hasAccess() {
        return Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager();
    }

    /** The system screen where the player allows this app to manage all files. */
    static Intent accessIntent(Context context) {
        return new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:" + context.getPackageName()));
    }

    /** Where logs and captures go by default: Documents/renut with access, the app's own Documents folder without. */
    static File outputDir(Context context) {
        File dir = hasAccess() ? documents() : new File(context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS), FOLDER);
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        return dir;
    }

    /**
     * The configuration file every option reads and writes: Documents/renut/renut.toml with access (made from the
     * one in the app's files the first time), the app's own file without.
     */
    static File configFile(Context context) {
        File own = new File(context.getFilesDir(), CONFIG);
        if (!hasAccess()) return own;
        File dir = documents();
        File shared = new File(dir, CONFIG);
        if (!shared.isFile()) {
            try {
                //noinspection ResultOfMethodCallIgnored
                dir.mkdirs();
                if (own.isFile()) Files.copy(own.toPath(), shared.toPath(), StandardCopyOption.REPLACE_EXISTING);
                else return own;
            } catch (IOException e) {
                return own;
            }
        }
        return shared;
    }

    /**
     * Before the runtime starts: the runtime reads files/renut.toml, so the configuration kept in Documents is
     * copied there, and it is told where to write its log (a file of its own per start) when the player asked.
     */
    static void prepareLaunch(Context context) {
        try {
            File source = configFile(context);
            File own = new File(context.getFilesDir(), CONFIG);
            if (!source.equals(own) && source.isFile()) {
                Files.copy(source.toPath(), own.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
            File target = new File(context.getFilesDir(), LOG_TARGET);
            if ("true".equals(RuntimeOptions.get(context, RuntimeOptions.LOG_TO_FILE, "false"))) {
                File log = new File(outputDir(context), "renut-" + stamp() + ".log");
                Files.write(target.toPath(), log.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
            } else {
                Files.deleteIfExists(target.toPath());
            }
            if ("true".equals(RuntimeOptions.get(context, RuntimeOptions.LOGCAT, "false"))) startLogcat(context);
        } catch (IOException ignored) { }
    }

    private static String stamp() {
        return new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
    }

    private static Process logcat;

    /**
     * Starts writing what logcat holds of this app from now on (every process of it, which is all an app may read)
     * to a file of its own in the output folder; the game's process calls it at its start and stopLogcat at its end.
     */
    static synchronized void startLogcat(Context context) {
        stopLogcat();
        try {
            File file = new File(outputDir(context), "logcat-" + stamp() + ".txt");
            String from = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US).format(new Date());
            logcat = new ProcessBuilder("logcat", "-v", "threadtime", "-T", from)
                .redirectErrorStream(true).redirectOutput(file).start();
        } catch (IOException ignored) { }
    }

    static synchronized void stopLogcat() {
        if (logcat != null) logcat.destroy();
        logcat = null;
    }
}
