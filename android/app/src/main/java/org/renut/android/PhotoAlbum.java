package org.renut.android;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Build;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * The pictures the game itself took (its Photo Mode). The game keeps each one inside a save
 * package under runtime-user/XUID/TITLEID/; this class only reads those packages, never writes
 * them. A photo is found by its image signature, so the album does not depend on which package
 * the game put it in, and a package without a picture is simply not listed.
 */
final class PhotoAlbum {
    private static final String TITLE_ID = "4D5307ED";
    private static final long MAX_FILE = 24L * 1024 * 1024;
    private static final int MIN_SIDE = 64;

    private PhotoAlbum() {}

    static final class Photo {
        final File file;
        final String title;
        final long takenAt;
        final int offset;
        final int width;
        final int height;

        Photo(File file, String title, long takenAt, int offset, int width, int height) {
            this.file = file;
            this.title = title;
            this.takenAt = takenAt;
            this.offset = offset;
            this.width = width;
            this.height = height;
        }

        /** Stable across reloads, for list keys. */
        String id() {
            return file.getPath() + "@" + offset;
        }
    }

    /** Changes whenever a package of the game is written; the screens reload on it. */
    static String stamp(File filesDir) {
        long newest = 0;
        int count = 0;
        File[] users = new File(filesDir, "runtime-user").listFiles();
        if (users == null) return "0";
        for (File user : users) {
            File[] kinds = new File(user, TITLE_ID).listFiles();
            if (kinds == null) continue;
            for (File kind : kinds) {
                File[] packages = kind.listFiles();
                if (packages == null) continue;
                for (File pack : packages) {
                    File[] files = pack.listFiles();
                    if (files == null) continue;
                    for (File file : files) {
                        count++;
                        newest = Math.max(newest, file.lastModified() ^ file.length());
                    }
                }
            }
        }
        return count + ":" + newest;
    }

    /** Every photo of the game, newest first. Reads the files, so call it off the main thread. */
    static List<Photo> load(File filesDir) {
        ArrayList<Photo> photos = new ArrayList<>();
        File[] users = new File(filesDir, "runtime-user").listFiles();
        if (users == null) return photos;
        for (File user : users) {
            File title = new File(user, TITLE_ID);
            File[] kinds = title.listFiles();
            if (kinds == null) continue;
            for (File kind : kinds) {
                // Headers hold only each package's display name; the data is in the others.
                if (!kind.isDirectory() || kind.getName().equals("Headers")) continue;
                File[] packages = kind.listFiles();
                if (packages == null) continue;
                for (File pack : packages) {
                    File[] files = pack.listFiles();
                    if (files == null) continue;
                    String name = displayName(new File(new File(title, "Headers"), kind.getName()), pack.getName());
                    for (File file : files) {
                        Photo photo = scan(file, name);
                        if (photo != null) photos.add(photo);
                    }
                }
            }
        }
        Collections.sort(photos, (a, b) -> Long.compare(b.takenAt, a.takenAt));
        return photos;
    }

    private static Photo scan(File file, String name) {
        if (!file.isFile() || file.length() < 2048 || file.length() > MAX_FILE) return null;
        byte[] data;
        try {
            data = readAll(file);
        } catch (IOException e) {
            return null;
        }
        int best = -1;
        long bestArea = 0;
        int bestW = 0, bestH = 0;
        for (int at = 0; at + 4 < data.length; at++) {
            if (!isSignature(data, at)) continue;
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(data, at, data.length - at, bounds);
            if (bounds.outWidth < MIN_SIDE || bounds.outHeight < MIN_SIDE) continue;
            long area = (long) bounds.outWidth * bounds.outHeight;
            // A package may hold a thumbnail next to the picture: the larger one is the photo.
            if (area > bestArea) {
                bestArea = area;
                best = at;
                bestW = bounds.outWidth;
                bestH = bounds.outHeight;
            }
        }
        if (best < 0) return null;
        return new Photo(file, name, file.lastModified(), best, bestW, bestH);
    }

    private static boolean isSignature(byte[] d, int at) {
        // JPEG: FF D8 FF + a marker byte; PNG: 89 'P' 'N' 'G'.
        if ((d[at] & 0xFF) == 0xFF && (d[at + 1] & 0xFF) == 0xD8 && (d[at + 2] & 0xFF) == 0xFF) {
            return (d[at + 3] & 0xFF) >= 0xC0;
        }
        return (d[at] & 0xFF) == 0x89 && d[at + 1] == 'P' && d[at + 2] == 'N' && d[at + 3] == 'G';
    }

    private static byte[] readAll(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            byte[] data = new byte[(int) file.length()];
            int off = 0;
            while (off < data.length) {
                int n = in.read(data, off, data.length - off);
                if (n < 0) break;
                off += n;
            }
            return off == data.length ? data : Arrays.copyOf(data, off);
        }
    }

    /** The package's display name from its header (UTF-16BE at byte 8), "" if it has none. */
    private static String displayName(File headers, String packageName) {
        File header = new File(headers, packageName + ".header");
        try (InputStream in = new FileInputStream(header)) {
            byte[] raw = new byte[128];
            int n = in.read(raw);
            if (n <= 8) return "";
            int end = 8;
            while (end + 1 < n && (raw[end] != 0 || raw[end + 1] != 0)) end += 2;
            return new String(raw, 8, end - 8, StandardCharsets.UTF_16BE).trim();
        } catch (IOException e) {
            return "";
        }
    }

    /** The picture at most maxSide px on its long edge, or null if the file no longer decodes. */
    static Bitmap decode(Photo photo, int maxSide) {
        try {
            byte[] data = readAll(photo.file);
            if (photo.offset >= data.length) return null;
            BitmapFactory.Options options = new BitmapFactory.Options();
            int longest = Math.max(photo.width, photo.height);
            int sample = 1;
            while (maxSide > 0 && longest / (sample * 2) >= maxSide) sample *= 2;
            options.inSampleSize = sample;
            return BitmapFactory.decodeByteArray(data, photo.offset, data.length - photo.offset, options);
        } catch (IOException | OutOfMemoryError e) {
            return null;
        }
    }

    /** Copies the photo to Pictures/reNut as a PNG. Returns whether it was written (Android 10+). */
    static boolean saveToGallery(ContentResolver resolver, Photo photo) {
        if (Build.VERSION.SDK_INT < 29) return false;
        Bitmap bitmap = decode(photo, 0);
        if (bitmap == null) return false;
        ContentValues values = new ContentValues();
        values.put(MediaStore.Images.Media.DISPLAY_NAME, "reNut-" + photo.takenAt + ".png");
        values.put(MediaStore.Images.Media.MIME_TYPE, "image/png");
        values.put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/reNut");
        values.put(MediaStore.Images.Media.IS_PENDING, 1);
        Uri uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
        if (uri == null) return false;
        try (OutputStream out = resolver.openOutputStream(uri)) {
            if (out == null || !bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)) {
                resolver.delete(uri, null, null);
                return false;
            }
        } catch (IOException e) {
            resolver.delete(uri, null, null);
            return false;
        }
        ContentValues done = new ContentValues();
        done.put(MediaStore.Images.Media.IS_PENDING, 0);
        resolver.update(uri, done, null, null);
        return true;
    }
}
