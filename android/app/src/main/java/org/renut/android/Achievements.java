package org.renut.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * The game's achievements as the launcher shows them. The runtime exports the catalog and the
 * icons (read from the user's own XEX) to files/achievements/ when it loads the game, and keeps
 * the unlocks in runtime-user/achievements/TITLEID.toml; this class only reads both.
 */
final class Achievements {
    // 100 ns intervals between 1601-01-01 (FILETIME) and 1970-01-01.
    private static final long EPOCH_DELTA = 116_444_736_000_000_000L;
    private static final Pattern UNLOCKED = Pattern.compile("\\[unlocked\\.(\\d+)\\]\\s*filetime\\s*=\\s*(\\d+)");
    private Achievements() {}

    static final class Item {
        final int id, gamerscore, image;
        final String label, description, lockedDescription;
        final File icon;
        /** Milliseconds since the Unix epoch, 0 while locked. */
        final long unlockedAt;
        Item(int id, String label, String description, String lockedDescription, int gamerscore, int image, File icon, long unlockedAt) {
            this.id = id; this.label = label; this.description = description; this.lockedDescription = lockedDescription;
            this.gamerscore = gamerscore; this.image = image; this.icon = icon; this.unlockedAt = unlockedAt;
        }
        boolean unlocked() { return unlockedAt > 0; }
        /** What to tell the player: secret achievements only carry their hint until they are won. */
        String shownDescription() { return unlocked() || lockedDescription.isEmpty() ? description : lockedDescription; }
    }

    static final class Snapshot {
        final List<Item> unlocked, locked;
        final int gamerscore, earnedGamerscore;
        Snapshot(List<Item> unlocked, List<Item> locked) {
            this.unlocked = unlocked; this.locked = locked;
            int total = 0, earned = 0;
            for (Item item : unlocked) { total += item.gamerscore; earned += item.gamerscore; }
            for (Item item : locked) total += item.gamerscore;
            gamerscore = total; earnedGamerscore = earned;
        }
        int count() { return unlocked.size() + locked.size(); }
    }

    private static File directory(File filesDir) { return new File(filesDir, "achievements"); }

    /** Changes whenever the catalog or the unlocks do; cheap enough to poll. */
    static long stamp(File filesDir) {
        long stamp = new File(directory(filesDir), "catalog.json").lastModified();
        File[] unlocks = new File(filesDir, "runtime-user/achievements").listFiles();
        if (unlocks != null) for (File file : unlocks) stamp += file.lastModified();
        return stamp;
    }

    /** Null while the game has never been loaded, so there is no catalog yet. */
    static Snapshot load(File filesDir) {
        File dir = directory(filesDir);
        JSONObject catalog = readCatalog(new File(dir, "catalog.json"));
        if (catalog == null) return null;
        Map<Integer, Long> unlocks = readUnlocks(unlockFile(filesDir, catalog.optString("titleId")));
        ArrayList<Item> unlocked = new ArrayList<>(), locked = new ArrayList<>();
        JSONArray entries = catalog.optJSONArray("achievements");
        for (int i = 0; entries != null && i < entries.length(); i++) {
            JSONObject entry = entries.optJSONObject(i);
            if (entry == null) continue;
            Item item = item(dir, entry, unlocks.get(entry.optInt("id")));
            (item.unlocked() ? unlocked : locked).add(item);
        }
        Collections.sort(unlocked, (a, b) -> Long.compare(b.unlockedAt, a.unlockedAt));
        return new Snapshot(unlocked, locked);
    }

    /** One achievement by id, for the pop-up; the unlock has just been written. */
    static Item find(File filesDir, int id) {
        File dir = directory(filesDir);
        JSONObject catalog = readCatalog(new File(dir, "catalog.json"));
        if (catalog == null) return null;
        JSONArray entries = catalog.optJSONArray("achievements");
        for (int i = 0; entries != null && i < entries.length(); i++) {
            JSONObject entry = entries.optJSONObject(i);
            if (entry != null && entry.optInt("id") == id)
                return item(dir, entry, readUnlocks(unlockFile(filesDir, catalog.optString("titleId"))).get(id));
        }
        return null;
    }

    private static Item item(File dir, JSONObject entry, Long filetime) {
        File icon = new File(dir, "icons/" + entry.optInt("image") + ".png");
        // The XEX's strings carry stray spaces and line breaks.
        return new Item(entry.optInt("id"), entry.optString("label").trim(), entry.optString("description").trim(),
            entry.optString("lockedDescription").trim(), entry.optInt("gamerscore"), entry.optInt("image"),
            icon.isFile() ? icon : null, filetime == null ? 0 : Math.max(1, (filetime - EPOCH_DELTA) / 10_000));
    }

    private static File unlockFile(File filesDir, String titleId) {
        return new File(filesDir, "runtime-user/achievements/" + titleId + ".toml");
    }

    private static JSONObject readCatalog(File file) {
        try {
            return new JSONObject(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    private static Map<Integer, Long> readUnlocks(File file) {
        HashMap<Integer, Long> unlocks = new HashMap<>();
        try {
            Matcher m = UNLOCKED.matcher(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
            while (m.find()) unlocks.put(Integer.parseInt(m.group(1)), Long.parseLong(m.group(2)));
        } catch (IOException | NumberFormatException ignored) {}
        return unlocks;
    }
}
