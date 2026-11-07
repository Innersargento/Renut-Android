package org.renut.android;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * The game's cover, read from the executable itself. The game writes it to achievements/title.png on
 * its first start; this does it at import time (same file) so the Play page has the picture from the
 * first launch of the app. The executable's resources hold an XDBF database whose title image is the
 * cover; the image is encrypted and "basic" compressed (the same layout tools/xex_basefile.py reads).
 */
final class GameCover {
    private static final byte[] RETAIL_KEY = {
        0x20, (byte) 0xB1, (byte) 0x85, (byte) 0xA5, (byte) 0x9D, 0x28, (byte) 0xFD, (byte) 0xC3,
        0x40, 0x58, 0x3F, (byte) 0xBB, 0x08, (byte) 0x96, (byte) 0xBF, (byte) 0x91};
    private static final int RESOURCE_INFO = 0x2FF, FILE_FORMAT = 0x3FF;

    private GameCover() {}

    static File file(Context context) { return new File(new File(context.getFilesDir(), "achievements"), "title.png"); }

    /** Writes the cover of [xex] unless the file is there already. Best effort: any failure leaves it as it was. */
    static void ensure(Context context, File xex) {
        File target = file(context);
        if (target.isFile() && target.length() > 0) return;
        try {
            byte[] png = read(xex);
            if (png == null) return;
            File directory = target.getParentFile();
            if (directory != null && !directory.isDirectory() && !directory.mkdirs()) return;
            File temporary = new File(directory, "title.png.tmp");
            Files.write(temporary.toPath(), png);
            Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
        } catch (Exception | OutOfMemoryError ignored) { }
    }

    /** The cover's PNG bytes, or null when the executable has none or is in a form this does not read. */
    static byte[] read(File xex) throws Exception {
        ByteBuffer file = ByteBuffer.wrap(Files.readAllBytes(xex.toPath()));
        if (file.getInt(0) != 0x58455832) return null; // "XEX2"
        int dataOffset = file.getInt(8), securityOffset = file.getInt(16), count = file.getInt(20);
        int resources = 0, format = 0;
        for (int i = 0; i < count; i++) {
            int key = file.getInt(24 + 8 * i), value = file.getInt(28 + 8 * i);
            if (key == RESOURCE_INFO) resources = value;
            else if (key == FILE_FORMAT) format = value;
        }
        if (resources == 0 || format == 0) return null;
        int infoSize = file.getInt(format), encryption = file.getShort(format + 4) & 0xFFFF,
            compression = file.getShort(format + 6) & 0xFFFF;
        int imageSize = file.getInt(securityOffset + 4), loadAddress = file.getInt(securityOffset + 0x110);
        if (compression > 1 || encryption > 1 || imageSize <= 0) return null;

        byte[] raw = file.array();
        int usable = (raw.length - dataOffset) / 16 * 16;
        byte[] data = new byte[raw.length - dataOffset];
        System.arraycopy(raw, dataOffset, data, 0, data.length);
        if (encryption == 1) {
            Cipher ecb = Cipher.getInstance("AES/ECB/NoPadding");
            ecb.init(Cipher.DECRYPT_MODE, new SecretKeySpec(RETAIL_KEY, "AES"));
            byte[] key = ecb.doFinal(raw, securityOffset + 0x150, 16);
            Cipher cbc = Cipher.getInstance("AES/CBC/NoPadding");
            cbc.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new IvParameterSpec(new byte[16]));
            cbc.doFinal(data, 0, usable, data, 0);
        }
        byte[] image;
        if (compression == 0) {
            image = data;
        } else {
            image = new byte[imageSize];
            int position = 0, out = 0;
            for (int i = 0; i < (infoSize - 8) / 8; i++) {
                int block = file.getInt(format + 8 + 8 * i), zeros = file.getInt(format + 12 + 8 * i);
                if (block < 0 || zeros < 0 || position + block > data.length || out + block + zeros > imageSize) return null;
                System.arraycopy(data, position, image, out, block);
                out += block + zeros;
                position += block;
            }
        }
        if (image.length < 2 || image[0] != 'M' || image[1] != 'Z') return null;

        // The resource named after the title id holds the XDBF database.
        int resourceCount = (file.getInt(resources) - 4) / 16;
        for (int i = 0; i < resourceCount; i++) {
            int at = resources + 4 + 16 * i;
            int address = file.getInt(at + 8), size = file.getInt(at + 12), start = address - loadAddress;
            if (start < 0 || size <= 24 || start + size > image.length) continue;
            ByteBuffer xdbf = ByteBuffer.wrap(image, start, size).slice();
            if (xdbf.getInt(0) != 0x58444246) continue; // "XDBF"
            int entries = xdbf.getInt(8), used = xdbf.getInt(12), freeEntries = xdbf.getInt(16);
            long base = 24L + entries * 18L + freeEntries * 8L;
            for (int j = 0; j < used; j++) {
                int entry = 24 + 18 * j;
                if ((xdbf.getShort(entry) & 0xFFFF) != 2 || xdbf.getLong(entry + 2) != 0x8000L) continue;
                long offset = base + (xdbf.getInt(entry + 10) & 0xFFFFFFFFL), length = xdbf.getInt(entry + 14) & 0xFFFFFFFFL;
                if (offset + length > size || length <= 0) return null;
                byte[] png = new byte[(int) length];
                System.arraycopy(image, start + (int) offset, png, 0, png.length);
                return png;
            }
        }
        return null;
    }
}
