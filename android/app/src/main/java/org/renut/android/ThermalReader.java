package org.renut.android;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Temperatures for the on-screen readout, in a way that does not depend on the SoC.
 *
 * Every Linux-based phone exposes its sensors as /sys/class/thermal/thermal_zone*, each with a
 * "type" naming it; the names differ by vendor (Qualcomm "cpu-1-5", "cpuss-0", "gpuss-3";
 * Exynos and Tensor "BIG", "MID", "LITTLE", "G3D"; MediaTek "mtktscpu", "mtktsgpu", "cpu_big",
 * "soc"; Unisoc, HiSilicon and others "cpu_thermal", "gpu_thermal"...). The zones are sorted by
 * what their name says, and only those that answer with a believable value are kept. The battery
 * comes from the system's own broadcast, which every device provides, so there is always a
 * number. A zone the app may not read (some vendors close them) is simply left out.
 */
final class ThermalReader {
    private static final String[] GPU_NAMES = {"gpu", "g3d", "mali", "adreno", "pvr", "powervr", "sgx", "gpuss"};
    private static final String[] CPU_NAMES = {"cpu", "big", "mid", "little", "prime", "kryo", "apc", "cluster",
            "core", "a55", "a76", "a78", "a510", "a710", "x1", "x2", "x3", "x4", "cortex"};
    private static final String[] SOC_NAMES = {"soc", "ap_thermal", "ap-thermal", "tsens", "ap_ntc", "ap", "chip"};

    private final Context context;
    private final List<File> cpu = new ArrayList<>(), gpu = new ArrayList<>(), soc = new ArrayList<>();

    ThermalReader(Context context) {
        this.context = context.getApplicationContext();
        File[] zones = new File("/sys/class/thermal").listFiles((dir, name) -> name.startsWith("thermal_zone"));
        if (zones == null) return;
        for (File zone : zones) {
            String type = readText(new File(zone, "type"));
            File temp = new File(zone, "temp");
            if (type == null || Double.isNaN(readTemperature(temp))) continue;  // closed or not a temperature
            String name = type.toLowerCase(Locale.ROOT);
            if (containsAny(name, GPU_NAMES)) gpu.add(temp);
            else if (containsAny(name, CPU_NAMES)) cpu.add(temp);
            else if (containsAny(name, SOC_NAMES) && !name.contains("skin")) soc.add(temp);
        }
    }

    private static boolean containsAny(String name, String[] words) {
        for (String word : words) {
            // Short words ("ap", "x1") only count as a whole token, or "cpuss" would match "ap" inside "map".
            if (word.length() <= 3) {
                for (String token : name.split("[^a-z0-9]+")) if (token.equals(word)) return true;
            } else if (name.contains(word)) {
                return true;
            }
        }
        return false;
    }

    private static String readText(File file) {
        try {
            return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).trim();
        } catch (IOException | SecurityException e) {
            return null;
        }
    }

    /** Degrees Celsius, or NaN when the file is unreadable or holds a value no sensor would report. */
    static double readTemperature(File file) {
        String text = readText(file);
        if (text == null || text.isEmpty()) return Double.NaN;
        double value;
        try {
            value = Double.parseDouble(text);
        } catch (NumberFormatException e) {
            return Double.NaN;
        }
        // Kernels report millidegrees (most), tenths of a degree (some MediaTek) or whole degrees.
        if (Math.abs(value) >= 1000) value /= 1000.0;
        else if (value >= 150) value /= 10.0;
        return value > 5 && value < 130 ? value : Double.NaN;
    }

    private static double hottest(List<File> files) {
        double best = Double.NaN;
        for (File file : files) {
            double value = readTemperature(file);
            if (!Double.isNaN(value) && (Double.isNaN(best) || value > best)) best = value;
        }
        return best;
    }

    /** What the readout can show on this device, e.g. "CPU, GPU, BAT"; for the settings and the report. */
    String sources() {
        StringBuilder out = new StringBuilder();
        if (!cpu.isEmpty()) out.append("CPU");
        else if (!soc.isEmpty()) out.append("SoC");
        if (!gpu.isEmpty()) out.append(out.length() > 0 ? ", " : "").append("GPU");
        out.append(out.length() > 0 ? ", " : "").append("BAT");
        return out.toString();
    }

    private double battery() {
        Intent intent = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (intent == null) return Double.NaN;
        int tenths = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
        return tenths == Integer.MIN_VALUE ? Double.NaN : tenths / 10.0;
    }

    /** One line such as "CPU 52°  GPU 48°  BAT 36°", leaving out what this device does not offer. */
    String read() {
        StringBuilder out = new StringBuilder();
        double c = hottest(cpu);
        if (Double.isNaN(c)) c = hottest(soc);
        if (!Double.isNaN(c)) append(out, cpu.isEmpty() ? "SoC" : "CPU", c);
        double g = hottest(gpu);
        if (!Double.isNaN(g)) append(out, "GPU", g);
        double b = battery();
        if (!Double.isNaN(b)) append(out, "BAT", b);
        return out.toString();
    }

    private static void append(StringBuilder out, String label, double value) {
        if (out.length() > 0) out.append("  ");
        out.append(label).append(' ').append(Math.round(value)).append('°');
    }
}
