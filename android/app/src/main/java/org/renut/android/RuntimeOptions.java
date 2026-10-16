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
    /** 0 full, 1 half, 2 quarter: how far the world's textures are reduced (plume/gpu). */
    static final String TEXTURE_QUALITY = "renut_plume_texture_quality";
    /** 0 full, 1 medium, 2 low: how many small shadow casters are left out (plume/gpu). */
    static final String SHADOW_QUALITY = "renut_plume_shadow_quality";
    /** 0 full, 1 three quarters, 2 half: the resolution the scene is drawn at (plume/gpu). */
    static final String INTERNAL_RESOLUTION = "renut_plume_internal_resolution";

    /**
     * The frame rate the game is held to (frame_pacing.h): 30 is its own, 31-240 a rate of the
     * player's choosing, 0 unlimited.
     */
    static final String FPS_TARGET = "renut_fps_target";
    /** The switch the slider replaced; still read from an older renut.toml. */
    private static final String LEGACY_UNCAP_FPS = "renut_plume_uncap_fps";
    static final int FPS_ORIGINAL = 30, FPS_MIN = 30, FPS_MAX = 240, FPS_STEP = 5, FPS_UNLIMITED = 0;
    /** The slider's positions: 30, 35 ... 240, then "unlimited" at the end (so 0..SLIDER_UNLIMITED). */
    static final int SLIDER_UNLIMITED = (FPS_MAX - FPS_MIN) / FPS_STEP + 1;

    /** The frame rate at a slider position. */
    static int fpsFromSlider(int position) {
        if (position >= SLIDER_UNLIMITED) return FPS_UNLIMITED;
        return FPS_MIN + Math.max(0, position) * FPS_STEP;
    }

    /** The slider position of a frame rate (the nearest step; 0 is the end of the bar). */
    static int sliderFromFps(int fps) {
        if (fps <= 0) return SLIDER_UNLIMITED;
        return Math.min(SLIDER_UNLIMITED - 1, Math.max(0, Math.round((fps - FPS_MIN) / (float) FPS_STEP)));
    }

    /** The saved frame-rate target; an old "unlock FPS" switch that was on reads as unlimited. */
    static int fpsTarget(Context context) {
        String value = get(context, FPS_TARGET, "");
        if (!value.isEmpty()) {
            try {
                int fps = Integer.parseInt(value);
                return fps <= 0 ? FPS_UNLIMITED : Math.min(FPS_MAX, Math.max(FPS_MIN, fps));
            } catch (NumberFormatException ignored) { }
        }
        return "true".equals(get(context, LEGACY_UNCAP_FPS, "false")) ? FPS_UNLIMITED : FPS_ORIGINAL;
    }

    private static final Set<String> BOOLEANS = new java.util.HashSet<>(java.util.Arrays.asList(
        "renut_plume_async_pipelines", LEGACY_UNCAP_FPS, "renut_plume_no_shadows", "renut_plume_no_bloom",
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
        boolean fps = false;
        if (key.equals(FPS_TARGET)) {
            try {
                int parsed = Integer.parseInt(value);
                fps = parsed == FPS_UNLIMITED || parsed >= FPS_MIN && parsed <= FPS_MAX;
            } catch (NumberFormatException ignored) { }
        }
        if (!(fps || BOOLEANS.contains(key) && (value.equals("true") || value.equals("false"))
            || key.equals("log_level") && LEVELS.contains(value)
            || (key.equals(TEXTURE_QUALITY) || key.equals(SHADOW_QUALITY) || key.equals(INTERNAL_RESOLUTION))
                && (value.equals("0") || value.equals("1") || value.equals("2"))
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
        // The slider has the last word: an old switch left on would turn 30 into unlimited.
        if (key.equals(FPS_TARGET) && !"false".equals(get(context, LEGACY_UNCAP_FPS, "false"))) {
            set(context, LEGACY_UNCAP_FPS, "false");
        }
    }

    private static File config(Context context) { return new File(context.getFilesDir(), "renut.toml"); }
}
