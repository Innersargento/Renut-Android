package org.renut.android;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.*;

/**
 * The pipeline recipes that ship inside the APK (assets/plume-recipes.seed), copied into the app's
 * files where the renderer reads them before the player's own list (plume/gpu/pipeline_cache.cpp, StartRecipes).
 * With them even the first start builds each scene's pipelines while its loading screen is up.
 * Copied again only when the APK changes.
 */
final class RecipeSeed {
    private static final String ASSET = "plume-recipes.seed";
    private static final String PREF = "recipeSeedApk";

    private RecipeSeed() {}

    static void sync(Context context) {
        try {
            long apk = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).lastUpdateTime;
            SharedPreferences prefs = context.getSharedPreferences("RecipeSeed", Context.MODE_PRIVATE);
            File target = new File(context.getFilesDir(), ASSET);
            if (target.isFile() && prefs.getLong(PREF, 0) == apk) return;
            File temp = new File(context.getFilesDir(), ASSET + ".tmp");
            try (InputStream in = context.getAssets().open(ASSET); OutputStream out = new FileOutputStream(temp)) {
                byte[] buffer = new byte[1 << 16];
                int n;
                while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
            }
            // Renamed only when complete: the renderer never reads half a list.
            if (!temp.renameTo(target)) throw new IOException("rename");
            prefs.edit().putLong(PREF, apk).apply();
        } catch (Exception ignored) {
            // No list in the APK: the player's own recordings still work from the second start.
        }
    }
}
