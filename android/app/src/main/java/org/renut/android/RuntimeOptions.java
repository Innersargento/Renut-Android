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
    private static final Set<String> BOOLEANS = Set.of(
        "renut_plume_async_pipelines");
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
        if (!(BOOLEANS.contains(key) && (value.equals("true") || value.equals("false"))
            || key.equals("log_level") && LEVELS.contains(value)))
            throw new IllegalArgumentException("Opção inválida: " + key);
        File file = config(context);
        List<String> lines = file.isFile()
            ? new ArrayList<>(Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) : new ArrayList<>();
        String assignment = key + " = " + (key.equals("log_level") ? "\"" + value + "\"" : value);
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
