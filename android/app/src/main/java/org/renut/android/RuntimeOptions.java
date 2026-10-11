package org.renut.android;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;

/** Edits only the runtime cvars exposed by the Android settings screen. */
final class RuntimeOptions {
    /** The cheats of cheats.cpp: a master switch, one flag per cheat and Banjo's outfit. */
    static final String CHEATS = "renut_cheats";
    static final String[] CHEAT_FLAGS = {
        "renut_cheat_infinite_health", "renut_cheat_infinite_fuel_ammo", "renut_cheat_infinite_parts",
        "renut_cheat_no_timer", "renut_cheat_no_notes_spent", "renut_cheat_vehicles_anywhere",
        "renut_cheat_extended_build_range"};
    static final String CHEAT_SKIN = "renut_cheat_banjo_skin";
    static final String[] SKINS = {"default", "robot", "tuxedo"};

    private static final Set<String> BOOLEANS = new java.util.HashSet<>(java.util.Arrays.asList(
        "renut_plume_async_pipelines", "renut_plume_uncap_fps", "renut_plume_no_shadows", "renut_plume_no_bloom",
        "renut_adpf", CHEATS));
    static {
        BOOLEANS.addAll(java.util.Arrays.asList(CHEAT_FLAGS));
    }
    private static final Set<String> LEVELS = Set.of("trace", "debug", "info", "warn", "error", "critical", "off");

    private RuntimeOptions() {}

    static String get(Context context, String key, String fallback) {
        try {
            for (String line : Files.readAllLines(config(context).toPath(), StandardCharsets.UTF_8)) {
                String trimmed = line.trim();
                if (trimmed.startsWith("#")) continue;
                int equal = trimmed.indexOf('=');
                if (equal < 0 || !trimmed.substring(0, equal).trim().equals(key)) continue;
                String value = trimmed.substring(equal + 1).trim();
                int comment = value.indexOf('#');
                if (comment >= 0) value = value.substring(0, comment).trim();
                if (value.length() >= 2 && value.startsWith("\"") && value.endsWith("\""))
                    value = value.substring(1, value.length() - 1);
                return value;
            }
        } catch (IOException ignored) { }
        return fallback;
    }

    static void set(Context context, String key, String value) throws IOException {
        boolean text = key.equals("log_level") || key.equals(CHEAT_SKIN);
        if (!(BOOLEANS.contains(key) && (value.equals("true") || value.equals("false"))
            || key.equals("log_level") && LEVELS.contains(value)
            || key.equals(CHEAT_SKIN) && java.util.Arrays.asList(SKINS).contains(value)))
            throw new IllegalArgumentException("Opção inválida: " + key);
        File file = config(context);
        List<String> lines = file.isFile()
            ? new ArrayList<>(Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) : new ArrayList<>();
        String assignment = key + " = " + (text ? "\"" + value + "\"" : value);
        boolean found = false;
        for (int i = 0; i < lines.size(); i++) {
            String trimmed = lines.get(i).trim();
            if (trimmed.startsWith("#")) continue;
            int equal = trimmed.indexOf('=');
            if (equal < 0 || !trimmed.substring(0, equal).trim().equals(key)) continue;
            if (!found) { lines.set(i, assignment); found = true; }
            else { lines.remove(i--); }
        }
        if (!found) lines.add(assignment);
        File temporary = new File(file.getParentFile(), file.getName() + ".tmp");
        Files.write(temporary.toPath(), lines, StandardCharsets.UTF_8);
        try { Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING); }
        catch (java.nio.file.AtomicMoveNotSupportedException ignored) {
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private static File config(Context context) { return new File(context.getFilesDir(), "renut.toml"); }
}
