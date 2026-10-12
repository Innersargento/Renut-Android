package org.renut.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The Turnip build that ships inside the APK (assets/turnip: the same ZIP layout the importer
 * takes, unpacked). On a supported Adreno it is unpacked once into the app's files and becomes the
 * driver used until the user picks another one.
 *
 * Turnip covers the Adreno 6xx, 7xx and 8xx generations; any other GPU keeps the system driver and
 * the bundled one is not offered at all.
 */
final class BundledDriver {
    private static final String ASSET = "turnip";
    /** Folder prefix in the files directory; also how a stored driver is recognised as the bundled one. */
    static final String DIR_PREFIX = "driver-bundled-";
    private static final String PREF_RENDERER = "gpuRenderer";
    /** Set when the user picks a driver themselves (including the system one), so the default stops applying. */
    static final String PREF_CHOSEN = "driverChosen";
    private static final Pattern ADRENO = Pattern.compile("Adreno.*?(\\d{3})");

    private BundledDriver() {}

    static boolean isBundledDir(String dir) {
        return dir != null && new File(dir).getName().startsWith(DIR_PREFIX);
    }

    /**
     * Makes sure the bundled driver is unpacked and listed, and selects it when the user has not
     * chosen anything else. Cheap after the first run: a prefs read and a file check.
     */
    static void sync(Context context, SharedPreferences prefs) {
        try {
            JSONObject meta = readMeta(context);
            if (meta == null || !supportedGpu(prefs)) return;
            String lib = meta.getString("libraryName");
            if (lib.contains("/") || !lib.endsWith(".so")) return;
            File files = context.getFilesDir();
            File dir = new File(files, DIR_PREFIX + tag(meta));
            if (!new File(dir, lib).isFile()) unpack(context, dir, lib);

            JSONArray array = new JSONArray(prefs.getString("drivers", "[]"));
            List<JSONObject> list = new ArrayList<>();
            List<File> stale = new ArrayList<>();
            boolean listed = false;
            for (int i = 0; i < array.length(); i++) {
                JSONObject d = array.getJSONObject(i);
                String path = d.optString("dir");
                if (d.optBoolean("bundled") && !path.equals(dir.getAbsolutePath())) {
                    stale.add(new File(path));  // an older build of the bundled driver
                    continue;
                }
                listed |= path.equals(dir.getAbsolutePath());
                list.add(d);
            }
            if (!listed) {
                list.add(new JSONObject().put("dir", dir.getAbsolutePath()).put("lib", lib)
                        .put("name", meta.optString("name", lib).trim()).put("bundled", true));
            }
            SharedPreferences.Editor edit = prefs.edit().putString("drivers", new JSONArray(list).toString());
            String selected = prefs.getString("driverDir", "");
            boolean selectedStale = false;
            for (File f : stale) selectedStale |= f.getAbsolutePath().equals(selected);
            // Default on, unless the user chose a driver (an imported one, or the system one).
            if (selected.isEmpty() ? !prefs.getBoolean(PREF_CHOSEN, false) : selectedStale) {
                edit.putString("driverDir", dir.getAbsolutePath()).putString("driverLib", lib);
            }
            edit.apply();
            for (File f : stale) {
                if (f.getName().startsWith(DIR_PREFIX)) {
                    try { GameFiles.removeTree(f); } catch (IOException ignored) {}
                }
            }
        } catch (Exception ignored) {
            // Nothing bundled or not unpackable: the system driver keeps working.
        }
    }

    private static JSONObject readMeta(Context context) {
        try (InputStream in = context.getAssets().open(ASSET + "/meta.json")) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            int n;
            while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
            return new JSONObject(out.toString(StandardCharsets.UTF_8.name()));
        } catch (Exception e) {
            return null;
        }
    }

    /** Folder suffix that changes with the driver build, so a new APK unpacks its own copy. */
    private static String tag(JSONObject meta) {
        String name = meta.optString("name", "turnip") + "-" + meta.optString("packageVersion", "0");
        return name.replaceAll("[^A-Za-z0-9.]+", "-");
    }

    private static void unpack(Context context, File dir, String lib) throws IOException {
        File temp = new File(dir.getParentFile(), dir.getName() + ".tmp");
        GameFiles.removeTree(temp);
        if (!temp.mkdirs()) throw new IOException("mkdir");
        try (InputStream in = context.getAssets().open(ASSET + "/" + lib);
             OutputStream out = new FileOutputStream(new File(temp, lib))) {
            byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
        }
        // Renamed only when complete: an existing folder is always a whole driver.
        GameFiles.removeTree(dir);
        if (!temp.renameTo(dir)) throw new IOException("rename");
    }

    /** Turnip needs an Adreno 6xx, 7xx or 8xx; the renderer string is read once and kept. */
    private static boolean supportedGpu(SharedPreferences prefs) {
        String renderer = prefs.getString(PREF_RENDERER, "");
        if (renderer.isEmpty()) {
            renderer = glRenderer();
            if (!renderer.isEmpty()) prefs.edit().putString(PREF_RENDERER, renderer).apply();
        }
        Matcher m = ADRENO.matcher(renderer);
        if (!m.find()) return false;
        int model = Integer.parseInt(m.group(1));
        return model >= 600 && model < 900;
    }

    private static String glRenderer() {
        try {
            EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            int[] version = new int[2];
            if (display == EGL14.EGL_NO_DISPLAY || !EGL14.eglInitialize(display, version, 0, version, 1)) return "";
            // The display is left initialised: it is reference counted and shared with the UI.
            int[] attrs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE};
            EGLConfig[] config = new EGLConfig[1];
            int[] count = new int[1];
            if (!EGL14.eglChooseConfig(display, attrs, 0, config, 0, 1, count, 0) || count[0] == 0) return "";
            EGLContext context = EGL14.eglCreateContext(display, config[0], EGL14.EGL_NO_CONTEXT,
                    new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            if (context == EGL14.EGL_NO_CONTEXT) return "";
            EGLSurface surface = EGL14.eglCreatePbufferSurface(display, config[0],
                    new int[]{EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
            String renderer = "";
            try {
                if (surface != EGL14.EGL_NO_SURFACE && EGL14.eglMakeCurrent(display, surface, surface, context)) {
                    String value = GLES20.glGetString(GLES20.GL_RENDERER);
                    if (value != null) renderer = value;
                }
            } finally {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface);
                EGL14.eglDestroyContext(display, context);
            }
            return renderer;
        } catch (Throwable e) {
            return "";
        }
    }
}
