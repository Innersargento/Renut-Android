package org.renut.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.File;
import org.json.JSONArray;
import org.json.JSONObject;

/** Android gamepad bindings, stored separately for each device descriptor. */
final class ExternalController {
    static final int BUTTON_COUNT = 14, TRIGGER_START = 14, AXIS_START = 16, COUNT = 20;
    /** Name of control {@code index}, in the app's language (the order of R.array.controller_labels). */
    static String label(int index) { return Language.array(R.array.controller_labels)[index]; }
    static final int[] MASKS = {
        GamepadOverlay.UP, GamepadOverlay.DOWN, GamepadOverlay.LEFT, GamepadOverlay.RIGHT,
        GamepadOverlay.START, GamepadOverlay.BACK, GamepadOverlay.L3, GamepadOverlay.R3,
        GamepadOverlay.LB, GamepadOverlay.RB, GamepadOverlay.A, GamepadOverlay.B,
        GamepadOverlay.X, GamepadOverlay.Y
    };
    private static final String PREFS = "external-controller";
    private static final String SELECTED = "selected";

    static final class Binding {
        final boolean key;
        final int code, sign;
        Binding(boolean key, int code, int sign) {
            this.key = key; this.code = code; this.sign = sign;
        }
        String label() {
            if (key) return KeyEvent.keyCodeToString(code).replace("KEYCODE_", "");
            return Language.s(R.string.ctrl_axis, MotionEvent.axisToString(code).replace("AXIS_", ""))
                + (sign < 0 ? " −" : " +");
        }
    }

    static final class Profile {
        final Binding[] bindings = new Binding[COUNT];
        String describe(int index) {
            return bindings[index] == null ? Language.s(R.string.ctrl_unassigned) : bindings[index].label();
        }
    }

    static boolean isGamepad(InputDevice device) {
        return device != null && ((device.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (device.getSources() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK);
    }

    static String descriptor(InputDevice device) {
        String value = device.getDescriptor();
        return value == null || value.isEmpty()
            ? device.getName() + ":" + device.getVendorId() + ":" + device.getProductId() : value;
    }

    static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** The controller whose mapping the mapping screen shows (not who plays as whom, see {@link #assign}). */
    static String selected(Context context) { return prefs(context).getString(SELECTED, ""); }
    static void select(Context context, InputDevice device) {
        prefs(context).edit().putString(SELECTED, descriptor(device)).apply();
    }

    // ---- who plays as whom -------------------------------------------------------------------
    // The game sees two local players: the first is the touch screen plus one gamepad, the second a
    // gamepad of its own. Which gamepad is which is chosen in the sidebar (game process) and in the
    // controller screen (app process), so it lives in a file both read fresh, not in SharedPreferences.

    static final int PLAYERS = 2;
    static final int PLAYER_ONE = 0, PLAYER_TWO = 1, NOT_USED = -1;
    private static final String ASSIGNMENT_FILE = "controller-players.json";

    /** A connected gamepad and the player it plays as ({@link #PLAYER_ONE}, {@link #PLAYER_TWO}, {@link #NOT_USED}). */
    static final class Pad {
        final InputDevice device;
        final int role;
        /** The device's name, with a number when two connected pads are called the same. */
        final String name;
        Pad(InputDevice device, int role, String name) { this.device = device; this.role = role; this.name = name; }
    }

    private static final class Choice {
        final String[] player = {"", ""};  // "" = automatic, else the descriptor asked for
        final java.util.LinkedHashSet<String> ignored = new java.util.LinkedHashSet<>();
        boolean rumble = true;
    }

    private static File assignmentFile(Context context) { return new File(context.getFilesDir(), ASSIGNMENT_FILE); }

    private static Choice readChoice(Context context) {
        Choice choice = new Choice();
        try {
            byte[] bytes = java.nio.file.Files.readAllBytes(assignmentFile(context).toPath());
            JSONObject json = new JSONObject(new String(bytes, java.nio.charset.StandardCharsets.UTF_8));
            choice.player[0] = json.optString("p1", "");
            choice.player[1] = json.optString("p2", "");
            choice.rumble = json.optBoolean("rumble", true);
            JSONArray ignored = json.optJSONArray("ignored");
            if (ignored != null) for (int i = 0; i < ignored.length(); i++) choice.ignored.add(ignored.optString(i));
        } catch (Exception ignored) { /* none yet, or unreadable: everything automatic */ }
        return choice;
    }

    private static void writeChoice(Context context, Choice choice) {
        try {
            JSONObject json = new JSONObject();
            json.put("p1", choice.player[0]);
            json.put("p2", choice.player[1]);
            json.put("ignored", new JSONArray(choice.ignored));
            json.put("rumble", choice.rumble);
            File target = assignmentFile(context), temporary = new File(target.getPath() + ".tmp");
            java.nio.file.Files.write(temporary.toPath(), json.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            java.nio.file.Files.move(temporary.toPath(), target.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        } catch (Exception ignored) { /* the choice is only lost, nothing else depends on it */ }
    }

    /** Every connected gamepad, in the order they were plugged in. */
    static java.util.List<InputDevice> connected() {
        int[] ids = InputDevice.getDeviceIds();
        java.util.Arrays.sort(ids);
        java.util.ArrayList<InputDevice> pads = new java.util.ArrayList<>();
        for (int id : ids) {
            InputDevice device = InputDevice.getDevice(id);
            if (isGamepad(device)) pads.add(device);
        }
        return pads;
    }

    /**
     * The gamepad each player uses, null for none. A choice made by the player wins while that pad is
     * connected; whatever is left over is handed out in plug-in order, so two pads just work as
     * player one and player two without opening any menu.
     */
    static InputDevice[] assign(Context context) {
        Choice choice = readChoice(context);
        java.util.ArrayList<InputDevice> free = new java.util.ArrayList<>();
        for (InputDevice device : connected()) if (!choice.ignored.contains(descriptor(device))) free.add(device);
        InputDevice[] result = new InputDevice[PLAYERS];
        for (int player = 0; player < PLAYERS; player++) {
            if (choice.player[player].isEmpty()) continue;
            for (InputDevice device : free) {
                if (descriptor(device).equals(choice.player[player])) { result[player] = device; break; }
            }
            if (result[player] != null) free.remove(result[player]);
        }
        for (int player = 0; player < PLAYERS; player++) {
            if (result[player] == null && !free.isEmpty()) result[player] = free.remove(0);
        }
        return result;
    }

    /** The connected gamepads with the player each plays as, for the menus. */
    static java.util.List<Pad> pads(Context context) {
        InputDevice[] assigned = assign(context);
        java.util.List<InputDevice> all = connected();
        java.util.ArrayList<Pad> pads = new java.util.ArrayList<>();
        for (InputDevice device : all) {
            int role = NOT_USED;
            for (int player = 0; player < PLAYERS; player++) {
                if (assigned[player] != null && assigned[player].getId() == device.getId()) role = player;
            }
            int same = 0, index = 0;
            for (InputDevice other : all) {
                if (!other.getName().equals(device.getName())) continue;
                same++;
                if (other.getId() <= device.getId()) index++;
            }
            pads.add(new Pad(device, role, same > 1 ? device.getName() + " #" + index : device.getName()));
        }
        return pads;
    }

    /** Makes {@code device} play as {@code role}. The other player keeps its own pad or goes back to automatic. */
    static void setRole(Context context, InputDevice device, int role) {
        Choice choice = readChoice(context);
        String key = descriptor(device);
        choice.ignored.remove(key);
        for (int player = 0; player < PLAYERS; player++) if (choice.player[player].equals(key)) choice.player[player] = "";
        if (role == PLAYER_ONE || role == PLAYER_TWO) choice.player[role] = key;
        else choice.ignored.add(key);
        writeChoice(context, choice);
    }

    /** Whether the game's vibration is played at all (on the gamepads, or the phone when a pad has no motors). */
    static boolean rumbleEnabled(Context context) { return readChoice(context).rumble; }
    static void setRumbleEnabled(Context context, boolean enabled) {
        Choice choice = readChoice(context);
        choice.rumble = enabled;
        writeChoice(context, choice);
    }

    /** Whether a second gamepad is playing: the game then sees two local players. */
    static boolean twoPlayers(Context context) { return assign(context)[PLAYER_TWO] != null; }

    static Profile automatic(InputDevice device) {
        Profile profile = new Profile();
        int[] keys = {
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y
        };
        for (int i = 0; i < BUTTON_COUNT; i++) profile.bindings[i] = new Binding(true, keys[i], 1);
        if (hasAxis(device, MotionEvent.AXIS_HAT_Y)) {
            profile.bindings[0] = new Binding(false, MotionEvent.AXIS_HAT_Y, -1);
            profile.bindings[1] = new Binding(false, MotionEvent.AXIS_HAT_Y, 1);
        }
        if (hasAxis(device, MotionEvent.AXIS_HAT_X)) {
            profile.bindings[2] = new Binding(false, MotionEvent.AXIS_HAT_X, -1);
            profile.bindings[3] = new Binding(false, MotionEvent.AXIS_HAT_X, 1);
        }
        profile.bindings[14] = trigger(device, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE, KeyEvent.KEYCODE_BUTTON_L2);
        profile.bindings[15] = trigger(device, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS, KeyEvent.KEYCODE_BUTTON_R2);
        profile.bindings[16] = axis(device, MotionEvent.AXIS_X, 1);
        profile.bindings[17] = axis(device, MotionEvent.AXIS_Y, -1);
        profile.bindings[18] = axis(device, hasAxis(device, MotionEvent.AXIS_RX) ? MotionEvent.AXIS_RX : MotionEvent.AXIS_Z, 1);
        profile.bindings[19] = axis(device, hasAxis(device, MotionEvent.AXIS_RY) ? MotionEvent.AXIS_RY : MotionEvent.AXIS_RZ, -1);
        return profile;
    }

    private static Binding trigger(InputDevice device, int preferred, int alternative, int key) {
        if (hasAxis(device, preferred)) return new Binding(false, preferred, 1);
        if (hasAxis(device, alternative)) return new Binding(false, alternative, 1);
        return new Binding(true, key, 1);
    }
    private static Binding axis(InputDevice device, int code, int sign) {
        return hasAxis(device, code) ? new Binding(false, code, sign) : null;
    }
    static boolean hasAxis(InputDevice device, int axis) {
        return device.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK) != null;
    }

    static Profile load(Context context, InputDevice device) {
        Profile profile = automatic(device);
        String json = prefs(context).getString("profile:" + descriptor(device), null);
        if (json == null) return profile;
        try {
            JSONArray values = new JSONArray(json);
            for (int i = 0; i < COUNT && i < values.length(); i++) {
                if (values.isNull(i)) { profile.bindings[i] = null; continue; }
                JSONObject value = values.getJSONObject(i);
                profile.bindings[i] = new Binding(value.getBoolean("key"), value.getInt("code"), value.getInt("sign"));
            }
        } catch (Exception ignored) { return automatic(device); }
        return profile;
    }

    static void save(Context context, InputDevice device, Profile profile) {
        JSONArray values = new JSONArray();
        for (Binding binding : profile.bindings) {
            if (binding == null) { values.put(JSONObject.NULL); continue; }
            JSONObject value = new JSONObject();
            try {
                value.put("key", binding.key);
                value.put("code", binding.code);
                value.put("sign", binding.sign);
            } catch (Exception impossible) { throw new IllegalStateException(impossible); }
            values.put(value);
        }
        prefs(context).edit().putString("profile:" + descriptor(device), values.toString()).apply();
    }

    static void reset(Context context, InputDevice device) {
        prefs(context).edit().remove("profile:" + descriptor(device)).apply();
    }

    interface Sink {
        void button(int mask, boolean down);
        void stick(int stick, float x, float y);
        void trigger(int trigger, float value);
    }

    static final class Player {
        private final InputDevice device;
        private final Profile profile;
        private final Sink sink;
        private final boolean[] buttons = new boolean[BUTTON_COUNT];
        private final float[] sticks = new float[4], triggers = new float[2];
        Player(InputDevice device, Profile profile, Sink sink) {
            this.device = device; this.profile = profile; this.sink = sink;
        }
        /** By the runtime id, not the descriptor: two identical pads can share a descriptor. */
        boolean accepts(InputDevice eventDevice) {
            return eventDevice != null && eventDevice.getId() == device.getId() && isGamepad(eventDevice);
        }
        int deviceId() { return device.getId(); }
        InputDevice device() { return device; }
        boolean key(KeyEvent event) {
            if (!accepts(event.getDevice())) return false;
            if (event.getAction() != KeyEvent.ACTION_DOWN && event.getAction() != KeyEvent.ACTION_UP) return false;
            boolean handled = false, down = event.getAction() == KeyEvent.ACTION_DOWN;
            for (int i = 0; i < COUNT; i++) {
                Binding binding = profile.bindings[i];
                if (binding == null || !binding.key || binding.code != event.getKeyCode()) continue;
                handled = true;
                if (i < BUTTON_COUNT) setButton(i, down);
                else if (i < AXIS_START) setTrigger(i - TRIGGER_START, down ? 1 : 0);
            }
            return handled;
        }
        boolean motion(MotionEvent event) {
            if (!accepts(event.getDevice())
                || (event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) return false;
            for (int i = 0; i < COUNT; i++) {
                Binding binding = profile.bindings[i];
                if (binding == null || binding.key) continue;
                float raw = event.getAxisValue(binding.code);
                if (i < BUTTON_COUNT) setButton(i, raw * binding.sign > 0.5f);
                else if (i < AXIS_START) setTrigger(i - TRIGGER_START, triggerValue(binding, raw));
                else sticks[i - AXIS_START] = Math.abs(raw) < 0.12f ? 0 : Math.max(-1, Math.min(1, raw * binding.sign));
            }
            sink.stick(0, sticks[0], sticks[1]);
            sink.stick(1, sticks[2], sticks[3]);
            return true;
        }
        private float triggerValue(Binding binding, float raw) {
            if (binding.sign < 0) return Math.max(0, Math.min(1, -raw));
            InputDevice.MotionRange range = device.getMotionRange(binding.code, InputDevice.SOURCE_JOYSTICK);
            if (range == null || range.getMax() <= range.getMin()) return Math.max(0, Math.min(1, raw));
            float value = (raw - range.getMin()) / (range.getMax() - range.getMin());
            return Math.max(0, Math.min(1, value));
        }
        private void setButton(int index, boolean down) {
            if (buttons[index] == down) return;
            buttons[index] = down; sink.button(MASKS[index], down);
        }
        private void setTrigger(int index, float value) {
            if (triggers[index] == value) return;
            triggers[index] = value; sink.trigger(index, value);
        }
        void release() {
            for (int i = 0; i < BUTTON_COUNT; i++) setButton(i, false);
            for (int i = 0; i < 2; i++) setTrigger(i, 0);
            for (int i = 0; i < 4; i++) sticks[i] = 0;
            sink.stick(0, 0, 0); sink.stick(1, 0, 0);
        }
    }
}
