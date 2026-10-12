package org.renut.android;

import android.content.Context;
import android.media.AudioAttributes;
import android.os.Build;
import android.os.CombinedVibration;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.InputDevice;
import java.util.ArrayList;

/**
 * The game's rumble, as the console's two motors per player (heavy left, light right), played on
 * the player's gamepad. A player whose gamepad has no motors, and the touch screen on its own,
 * rumble the phone instead, so vibration is never silently lost.
 */
final class Rumble {
    /** Length of each vibration sent; it is sent again while the game keeps asking. */
    private static final long SHOT_MS = 4000;
    /** A phone motor without amplitude control only runs on or off: below this the rumble is too weak to be worth it. */
    private static final int PHONE_ON_OFF_THRESHOLD = 0x4D00;

    private final Vibrator phone;
    private final AudioAttributes usage = new AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build();
    private final ArrayList<InputDevice> touched = new ArrayList<>();
    private long applied;
    private long appliedAt;
    private boolean phoneOn;

    Rumble(Context context) {
        Vibrator found = null;
        if (Build.VERSION.SDK_INT >= 31) {
            VibratorManager manager = (VibratorManager) context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            if (manager != null) found = manager.getDefaultVibrator();
        } else {
            found = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
        }
        phone = found != null && found.hasVibrator() ? found : null;
    }

    private static int amplitude(int speed) {
        return Math.max(1, Math.min(255, speed * 255 / 0xFFFF));
    }

    private static VibrationEffect effect(int speed, boolean amplitudeControl) {
        return VibrationEffect.createOneShot(SHOT_MS,
            amplitudeControl ? amplitude(speed) : VibrationEffect.DEFAULT_AMPLITUDE);
    }

    /**
     * Plays {@code packed} (rumble_packed of the native side: left then right, 16 bits each, player
     * one in the low half) on {@code devices}, the gamepad of each player or null. Cheap to call
     * often: nothing is sent while the request stays the same, except to renew a long vibration.
     */
    void apply(long packed, InputDevice[] devices) {
        long now = SystemClock.uptimeMillis();
        if (packed == applied && (packed == 0 || now - appliedAt < SHOT_MS - 600)) return;
        int phoneSpeed = 0;
        for (int player = 0; player < ExternalController.PLAYERS; player++) {
            int left = (int) ((packed >>> (player * 32)) & 0xFFFF);
            int right = (int) ((packed >>> (player * 32 + 16)) & 0xFFFF);
            InputDevice device = devices[player];
            if (device != null && drive(device, left, right)) continue;
            phoneSpeed = Math.max(phoneSpeed, Math.max(left, right));
        }
        drivePhone(phoneSpeed);
        applied = packed;
        appliedAt = now;
    }

    /** Silences everything this has started; the next {@link #apply} starts from scratch. */
    void stop() {
        for (InputDevice device : touched) cancel(device);
        touched.clear();
        drivePhone(0);
        applied = 0;
    }

    private boolean knows(InputDevice device) {
        for (InputDevice known : touched) if (known.getId() == device.getId()) return true;
        return false;
    }

    private static void cancel(InputDevice device) {
        try {
            if (Build.VERSION.SDK_INT >= 31) device.getVibratorManager().cancel();
            else device.getVibrator().cancel();
        } catch (RuntimeException ignored) { /* the pad went away */ }
    }

    /** Runs the gamepad's motors. False when it has none (the phone takes over). */
    private boolean drive(InputDevice device, int left, int right) {
        try {
            boolean off = left == 0 && right == 0;
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager manager = device.getVibratorManager();
                int[] ids = manager.getVibratorIds();
                if (ids.length == 0) return false;
                if (off) {
                    if (knows(device)) manager.cancel();
                    return true;
                }
                CombinedVibration.ParallelCombination both = CombinedVibration.startParallel();
                if (ids.length == 1) {
                    both.addVibrator(ids[0], effect(Math.max(left, right), manager.getVibrator(ids[0]).hasAmplitudeControl()));
                } else {
                    if (left > 0) both.addVibrator(ids[0], effect(left, manager.getVibrator(ids[0]).hasAmplitudeControl()));
                    if (right > 0) both.addVibrator(ids[1], effect(right, manager.getVibrator(ids[1]).hasAmplitudeControl()));
                }
                manager.vibrate(both.combine());
            } else {
                Vibrator motor = device.getVibrator();
                if (motor == null || !motor.hasVibrator()) return false;
                if (off) {
                    if (knows(device)) motor.cancel();
                    return true;
                }
                motor.vibrate(effect(Math.max(left, right), motor.hasAmplitudeControl()));
            }
            if (!off && !knows(device)) touched.add(device);
            return true;
        } catch (RuntimeException e) {
            return false;
        }
    }

    private void drivePhone(int speed) {
        if (phone == null) return;
        try {
            boolean amplitudeControl = phone.hasAmplitudeControl();
            boolean on = speed > 0 && (amplitudeControl || speed >= PHONE_ON_OFF_THRESHOLD);
            if (!on) {
                if (phoneOn) phone.cancel();
                phoneOn = false;
                return;
            }
            phone.vibrate(effect(speed, amplitudeControl), usage);
            phoneOn = true;
        } catch (RuntimeException ignored) { /* vibration is a courtesy, never a reason to fail */ }
    }

    /** One short pulse on every player's gamepad (the phone for a player without one), for the menus. */
    static void test(Context context) {
        final Rumble rumble = new Rumble(context);
        InputDevice[] devices = new InputDevice[ExternalController.PLAYERS];
        for (ExternalController.Pad pad : ExternalController.pads(context)) {
            if (pad.role >= 0) devices[pad.role] = pad.device;
        }
        // A player with a pad pulses it; with none at all, the first player (the touch screen) pulses the phone.
        long packed = 0;
        for (int player = 0; player < ExternalController.PLAYERS; player++) {
            if (devices[player] != null) packed |= 0xFFFFFFFFL << (player * 32);
        }
        rumble.apply(packed != 0 ? packed : 0xFFFFFFFFL, devices);
        new Handler(Looper.getMainLooper()).postDelayed(rumble::stop, 700);
    }
}
