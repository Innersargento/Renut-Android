package org.renut.android;

import android.content.Context;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Where everything the game screen draws over the game sits: the on-screen controller (triggers,
 * bumpers, sticks, D-pad, face buttons, Back, Start) and the FPS and temperature readouts.
 *
 * A layout is a profile: each element has a centre (as a fraction of the screen, so a profile
 * fits any size), a scale, an opacity of its own and whether it is shown. An element a
 * profile has not moved sits where {@link #defaultShape} puts it, which is also what the
 * built-in "default" profile is. Profiles live in files/hud-layouts.json, a file rather than
 * preferences because the game runs in its own process and reads the choice at every start.
 */
public final class HudLayout {
    static final String LT = "lt", RT = "rt", LB = "lb", RB = "rb", STICK_L = "stick_l", DPAD = "dpad",
            STICK_R = "stick_r", Y = "y", X = "x", B = "b", A = "a", BACK = "back", START = "start",
            FPS = "fps", TEMP = "temp";
    static final String[] IDS = {LT, RT, LB, RB, STICK_L, DPAD, STICK_R, Y, X, B, A, BACK, START, FPS, TEMP};
    /** The profile nobody edits: every element where the controller's own geometry puts it. */
    static final String DEFAULT = "default";
    static final float MIN_SCALE = 0.5f, MAX_SCALE = 2.0f, MIN_OPACITY = 0.2f;

    private HudLayout() {}

    static boolean isReadout(String id) { return FPS.equals(id) || TEMP.equals(id); }

    // ---- model -------------------------------------------------------------------------------

    public static final class Item {
        boolean placed;       // false: at its default position
        float x, y;           // centre, fraction of the screen
        float scale = 1f;
        float opacity = 1f;   // 0.2 to 1, this element's own
        boolean visible = true;

        Item copy() {
            Item item = new Item();
            item.placed = placed; item.x = x; item.y = y; item.scale = scale; item.opacity = opacity;
            item.visible = visible;
            return item;
        }
    }

    public static final class Profile {
        String name;
        final Map<String, Item> items = new LinkedHashMap<>();

        Profile(String name) { this.name = name; }

        Item item(String id) {
            Item item = items.get(id);
            if (item == null) { item = new Item(); items.put(id, item); }
            return item;
        }

        boolean isDefault() { return DEFAULT.equals(name); }

        Profile copy(String newName) {
            Profile copy = new Profile(newName);
            for (Map.Entry<String, Item> entry : items.entrySet()) copy.items.put(entry.getKey(), entry.getValue().copy());
            return copy;
        }

        JSONObject toJson() throws JSONException {
            JSONObject out = new JSONObject();
            out.put("name", name);
            JSONObject list = new JSONObject();
            for (Map.Entry<String, Item> entry : items.entrySet()) {
                Item item = entry.getValue();
                JSONObject o = new JSONObject();
                if (item.placed) { o.put("x", item.x); o.put("y", item.y); }
                o.put("s", item.scale);
                o.put("o", item.opacity);
                o.put("v", item.visible);
                list.put(entry.getKey(), o);
            }
            out.put("items", list);
            return out;
        }

        static Profile fromJson(JSONObject json) {
            Profile profile = new Profile(json.optString("name", "?"));
            // Profiles saved before opacity was per element had one opacity for the whole pad:
            // it becomes each pad element's own (the readouts were never under it).
            float legacy = clamp((float) json.optDouble("opacity", 1.0), MIN_OPACITY, 1f);
            if (legacy < 1f) {
                for (String id : IDS) if (!isReadout(id)) profile.item(id).opacity = legacy;
            }
            JSONObject list = json.optJSONObject("items");
            if (list != null) {
                for (String id : IDS) {
                    JSONObject o = list.optJSONObject(id);
                    if (o == null) continue;
                    Item item = profile.item(id);
                    if (o.has("o")) item.opacity = clamp((float) o.optDouble("o", 1.0), MIN_OPACITY, 1f);
                    item.placed = o.has("x") && o.has("y");
                    if (item.placed) {
                        item.x = clamp((float) o.optDouble("x", 0.5), 0f, 1f);
                        item.y = clamp((float) o.optDouble("y", 0.5), 0f, 1f);
                    }
                    item.scale = clamp((float) o.optDouble("s", 1.0), MIN_SCALE, MAX_SCALE);
                    // Only the readouts can be hidden here; a pad control is always shown, whatever an
                    // older file says.
                    item.visible = !isReadout(id) || o.optBoolean("v", true);
                }
            }
            return profile;
        }
    }

    static float clamp(float v, float lo, float hi) { return Math.max(lo, Math.min(hi, v)); }

    // ---- geometry ----------------------------------------------------------------------------

    /** Where an element is on a w x h screen: a circle (r) or a rectangle (hw x hh half extents). */
    public static final class Shape {
        float cx, cy, r, hw, hh;
        boolean circle;

        boolean contains(float px, float py, float slop) {
            if (circle) {
                float dx = px - cx, dy = py - cy, reach = r + slop;
                return dx * dx + dy * dy <= reach * reach;
            }
            return Math.abs(px - cx) <= hw + slop && Math.abs(py - cy) <= hh + slop;
        }
    }

    private static Shape circle(float cx, float cy, float r) {
        Shape s = new Shape();
        s.circle = true; s.cx = cx; s.cy = cy; s.r = r;
        return s;
    }

    private static Shape rect(float cx, float cy, float hw, float hh) {
        Shape s = new Shape();
        s.cx = cx; s.cy = cy; s.hw = hw; s.hh = hh;
        return s;
    }

    /**
     * The pad's own layout: triggers and bumpers on top, the left stick high and the D-pad low on
     * the left, face buttons high and the right stick low on the right, Back and Start centred.
     * The readouts sit at the bottom centre, as the FPS counter always has.
     */
    static Shape defaultShape(String id, int w, int h) {
        float u = Math.min(w, h) / 10f;
        float side = Math.max(u * 0.4f, w * 0.02f);
        float fx = w - side - u * 2.1f, fy = h * 0.47f, gap = u * 1.05f, br = u * 0.62f;
        switch (id) {
            case LT: return rect(side + u * 1.1f, u * 0.8f, u * 1.1f, u * 0.5f);
            case RT: return rect(w - side - u * 1.1f, u * 0.8f, u * 1.1f, u * 0.5f);
            case LB: return rect(side + u * 1.1f, u * 1.95f, u * 1.1f, u * 0.4f);
            case RB: return rect(w - side - u * 1.1f, u * 1.95f, u * 1.1f, u * 0.4f);
            case STICK_L: return circle(side + u * 1.9f, h * 0.52f, u * 1.55f);
            case DPAD: return circle(side + u * 5.0f, h - u * 1.9f, u * 1.45f);
            case STICK_R: return circle(w - side - u * 5.0f, h - u * 1.9f, u * 1.45f);
            case Y: return circle(fx, fy - gap, br);
            case X: return circle(fx - gap, fy, br);
            case B: return circle(fx + gap, fy, br);
            case A: return circle(fx, fy + gap, br);
            case BACK: return rect(w / 2f - u * 1.95f, u * 0.95f, u * 0.95f, u * 0.38f);
            case START: return rect(w / 2f + u * 1.95f, u * 0.95f, u * 0.95f, u * 0.38f);
            case FPS: return rect(w / 2f, h - u * 0.45f, u * 0.9f, u * 0.3f);
            case TEMP: return rect(w / 2f, h - u * 1.05f, u * 1.7f, u * 0.3f);
            default: throw new IllegalArgumentException(id);
        }
    }

    /** The shape of an element under a profile: its default, moved and scaled as the profile says. */
    static Shape shape(Profile profile, String id, int w, int h) {
        Shape s = defaultShape(id, w, h);
        Item item = profile == null ? null : profile.items.get(id);
        if (item == null) return s;
        if (item.placed) { s.cx = item.x * w; s.cy = item.y * h; }
        s.r *= item.scale; s.hw *= item.scale; s.hh *= item.scale;
        return s;
    }

    static float opacity(Profile profile, String id) {
        Item item = profile == null ? null : profile.items.get(id);
        return item == null ? 1f : item.opacity;
    }

    /** A pad control is always visible; the FPS and temperature readouts can be switched off. */
    static boolean visible(Profile profile, String id) {
        if (!isReadout(id)) return true;
        Item item = profile == null ? null : profile.items.get(id);
        return item == null || item.visible;
    }

    // ---- store -------------------------------------------------------------------------------

    private static File file(Context context) { return new File(context.getFilesDir(), "hud-layouts.json"); }

    private static final class Store {
        String selected = DEFAULT;
        final List<Profile> profiles = new ArrayList<>();
    }

    private static Store read(Context context) {
        Store store = new Store();
        try {
            String text = new String(Files.readAllBytes(file(context).toPath()), StandardCharsets.UTF_8);
            JSONObject json = new JSONObject(text);
            store.selected = json.optString("selected", DEFAULT);
            JSONArray array = json.optJSONArray("profiles");
            if (array != null) {
                for (int i = 0; i < array.length(); i++) {
                    Profile p = Profile.fromJson(array.getJSONObject(i));
                    if (!p.isDefault() && find(store, p.name) == null) store.profiles.add(p);
                }
            }
        } catch (IOException | JSONException ignored) {
            // No file yet, or one that cannot be read: the built-in profile only.
        }
        if (!DEFAULT.equals(store.selected) && find(store, store.selected) == null) store.selected = DEFAULT;
        return store;
    }

    private static Profile find(Store store, String name) {
        for (Profile p : store.profiles) if (p.name.equals(name)) return p;
        return null;
    }

    private static void write(Context context, Store store) throws IOException {
        try {
            JSONObject json = new JSONObject();
            json.put("selected", store.selected);
            JSONArray array = new JSONArray();
            for (Profile p : store.profiles) array.put(p.toJson());
            json.put("profiles", array);
            GameFiles.atomicWrite(file(context), json.toString());
        } catch (JSONException e) {
            throw new IOException(e);
        }
    }

    /** Names of the saved profiles, the built-in one first (as {@link #DEFAULT}). */
    static List<String> names(Context context) {
        List<String> out = new ArrayList<>();
        out.add(DEFAULT);
        for (Profile p : read(context).profiles) out.add(p.name);
        return out;
    }

    static String selectedName(Context context) { return read(context).selected; }

    /** A copy of the selected profile, safe to edit. */
    static Profile load(Context context) {
        Store store = read(context);
        Profile found = DEFAULT.equals(store.selected) ? null : find(store, store.selected);
        return found != null ? found.copy(found.name) : new Profile(DEFAULT);
    }

    static Profile load(Context context, String name) {
        if (DEFAULT.equals(name)) return new Profile(DEFAULT);
        Profile found = find(read(context), name);
        return found != null ? found.copy(found.name) : new Profile(DEFAULT);
    }

    static void select(Context context, String name) throws IOException {
        Store store = read(context);
        store.selected = DEFAULT.equals(name) || find(store, name) != null ? name : DEFAULT;
        write(context, store);
    }

    /** Stores the profile under its name (replacing one of that name) and selects it. */
    static void save(Context context, Profile profile) throws IOException {
        if (profile.isDefault()) throw new IOException("default");
        Store store = read(context);
        Profile saved = profile.copy(profile.name);
        boolean replaced = false;
        for (int i = 0; i < store.profiles.size(); i++) {
            if (store.profiles.get(i).name.equals(saved.name)) { store.profiles.set(i, saved); replaced = true; }
        }
        if (!replaced) store.profiles.add(saved);
        store.selected = saved.name;
        write(context, store);
    }

    /** Deletes a profile; the built-in one is selected afterwards if it was the chosen one. */
    static void delete(Context context, String name) throws IOException {
        Store store = read(context);
        store.profiles.removeIf(p -> p.name.equals(name));
        if (store.selected.equals(name)) store.selected = DEFAULT;
        write(context, store);
    }
}
