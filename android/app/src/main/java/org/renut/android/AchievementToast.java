package org.renut.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Outline;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayDeque;

/**
 * The "achievement unlocked" banner over the game: slides in at the top, stays a few seconds and
 * leaves. It takes no touches, so the on-screen controls underneath keep working, and unlocks
 * that arrive together are shown one after the other.
 */
final class AchievementToast extends LinearLayout {
    private static final long SLIDE_MS = 320, STAY_MS = 4500;
    private final ArrayDeque<Achievements.Item> queue = new ArrayDeque<>();
    private final ImageView icon;
    private final TextView title, detail;
    private boolean showing;
    private final float density;

    AchievementToast(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        setPadding(dp(10), dp(10), dp(18), dp(10));
        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.rgb(30, 20, 12));
        background.setCornerRadius(dp(18));
        background.setStroke(dp(2), Color.rgb(255, 155, 69));
        setBackground(background);
        setElevation(dp(8));
        setClickable(false);
        setVisibility(GONE);
        icon = new ImageView(context);
        icon.setScaleType(ImageView.ScaleType.CENTER_CROP);
        icon.setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View view, Outline outline) { outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), dp(10)); }
        });
        icon.setClipToOutline(true);
        addView(icon, new LayoutParams(dp(52), dp(52)));
        LinearLayout text = new LinearLayout(context);
        text.setOrientation(VERTICAL);
        text.setPadding(dp(12), 0, 0, 0);
        TextView heading = new TextView(context);
        heading.setText(Language.s(R.string.ach_toast_heading));
        heading.setTextColor(Color.rgb(255, 155, 69));
        heading.setTextSize(11);
        heading.setTypeface(null, Typeface.BOLD);
        heading.setLetterSpacing(0.08f);
        text.addView(heading);
        title = line(context, 16, Color.rgb(244, 240, 235), true);
        text.addView(title);
        detail = line(context, 12, Color.rgb(168, 162, 156), false);
        text.addView(detail);
        addView(text, new LayoutParams(-2, -2));
    }

    private TextView line(Context context, int size, int color, boolean bold) {
        TextView view = new TextView(context);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setSingleLine();
        view.setEllipsize(TextUtils.TruncateAt.END);
        if (bold) view.setTypeface(null, Typeface.BOLD);
        return view;
    }

    private int dp(int value) { return Math.round(value * density); }

    /** Where it sits: top centre, clear of the on-screen controls. */
    static FrameLayout.LayoutParams params(Context context) {
        float density = context.getResources().getDisplayMetrics().density;
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        params.topMargin = Math.round(14 * density);
        return params;
    }

    /** Called on the UI thread. */
    void push(Achievements.Item item) {
        queue.add(item);
        if (!showing) next();
    }

    private void next() {
        Achievements.Item item = queue.poll();
        if (item == null) { showing = false; return; }
        showing = true;
        Bitmap bitmap = item.icon == null ? null : BitmapFactory.decodeFile(item.icon.getPath());
        if (bitmap != null) icon.setImageBitmap(bitmap); else icon.setImageDrawable(null);
        icon.setBackgroundColor(Color.rgb(56, 39, 26));
        title.setText(item.label);
        detail.setText(item.gamerscore > 0 ? item.gamerscore + " G · " + item.description : item.description);
        int widest = Math.min(dp(380), getResources().getDisplayMetrics().widthPixels - dp(32));
        setMinimumWidth(Math.min(widest, dp(280)));
        title.setMaxWidth(widest - dp(100));
        detail.setMaxWidth(widest - dp(100));
        setVisibility(VISIBLE);
        setAlpha(0f);
        setTranslationY(-dp(90));
        animate().alpha(1f).translationY(0f).setDuration(SLIDE_MS).withEndAction(() ->
            postDelayed(() -> animate().alpha(0f).translationY(-dp(90)).setDuration(SLIDE_MS).withEndAction(() -> {
                setVisibility(GONE);
                next();
            }).start(), STAY_MS)).start();
    }
}
