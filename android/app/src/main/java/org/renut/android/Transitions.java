package org.renut.android;

import android.app.Activity;
import android.os.Build;

/**
 * Screen-to-screen animations between activities. Each activity states how it enters (in its
 * onCreate) and how it leaves (in finish), so the pair always matches whichever screen opened it.
 * Inside an activity the Compose screens animate on their own (see pageTransition).
 */
final class Transitions {
    private Transitions() {}

    /** A screen that goes deeper: it comes in from the right, the previous one drifts left. */
    static void slideOpen(Activity activity) {
        open(activity, R.anim.slide_in_right, R.anim.slide_out_left);
    }

    /** Leaving a screen opened with slideOpen: the reverse movement. */
    static void slideClose(Activity activity) {
        close(activity, R.anim.slide_in_left, R.anim.slide_out_right);
    }

    /** For a change of context (launcher <-> game): a plain cross-fade. */
    static void fadeOpen(Activity activity) {
        open(activity, android.R.anim.fade_in, android.R.anim.fade_out);
    }

    static void fadeClose(Activity activity) {
        close(activity, android.R.anim.fade_in, android.R.anim.fade_out);
    }

    private static void open(Activity activity, int enter, int exit) {
        if (Build.VERSION.SDK_INT >= 34)
            activity.overrideActivityTransition(Activity.OVERRIDE_TRANSITION_OPEN, enter, exit);
        else
            activity.overridePendingTransition(enter, exit);
    }

    private static void close(Activity activity, int enter, int exit) {
        if (Build.VERSION.SDK_INT >= 34)
            activity.overrideActivityTransition(Activity.OVERRIDE_TRANSITION_CLOSE, enter, exit);
        else
            activity.overridePendingTransition(enter, exit);
    }
}
