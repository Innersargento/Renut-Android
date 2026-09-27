package org.renut.android;

import android.view.Surface;

/** Decodes the game's original ASF/WMV3 stream directly onto a surface. */
final class NativeMoviePlayer {
    static { System.loadLibrary("renut_video"); }
    private NativeMoviePlayer() {}
    /** Returns null on success or the failure; startNanos is System.nanoTime() of the game's first movie frame. */
    static native String play(String source, Surface surface, Thread worker, Object callbacks, long startNanos);
}
