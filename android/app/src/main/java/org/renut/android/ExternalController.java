package org.renut.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import org.json.JSONArray;
import org.json.JSONObject;

/** Android gamepad bindings, stored separately for each device descriptor. */
final class ExternalController {
    static final int BUTTON_COUNT = 14, TRIGGER_START = 14, AXIS_START = 16, COUNT = 20;
    static final String[] LABELS = {
        "Direcional cima", "Direcional baixo", "Direcional esquerda", "Direcional direita",
        "Start", "Back", "L3", "R3", "LB", "RB", "A", "B", "X", "Y",
        "Gatilho esquerdo", "Gatilho direito",
        "Analógico esquerdo →", "Analógico esquerdo ↑",
        "Analógico direito →", "Analógico direito ↑"
    };
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
            return "Eixo " + MotionEvent.axisToString(code).replace("AXIS_", "")
                + (sign < 0 ? " −" : " +");
        }
    }

    static final class Profile {
        final Binding[] bindings = new Binding[COUNT];
        String describe(int index) {
            return bindings[index] == null ? "Não atribuído" : bindings[index].label();
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

    static String selected(Context context) { return prefs(context).getString(SELECTED, ""); }
    static void select(Context context, InputDevice device) {
        prefs(context).edit().putString(SELECTED, descriptor(device)).apply();
    }

    static InputDevice findSelected(Context context) {
        String selected = selected(context);
        InputDevice first = null;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (!isGamepad(device)) continue;
            if (first == null) first = device;
            if (descriptor(device).equals(selected)) return device;
        }
        return selected.isEmpty() ? first : null;
    }

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
        boolean accepts(InputDevice eventDevice) {
            return isGamepad(eventDevice) && descriptor(device).equals(descriptor(eventDevice));
        }
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
