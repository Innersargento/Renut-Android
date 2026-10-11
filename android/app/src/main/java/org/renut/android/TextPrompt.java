package org.renut.android;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The text box the game gets when it asks for the Xbox keyboard (naming a blueprint, for
 * instance). It sits over the game, dims it, and answers through {@link Listener}: the text the
 * player typed, or a cancellation. The card is kept near the top so the soft keyboard, which
 * rises from the bottom of a landscape screen, does not cover it.
 */
final class TextPrompt extends FrameLayout {
    interface Listener {
        /** The player pressed OK (cancelled=false, text typed) or cancelled. */
        void onAnswer(String text, boolean cancelled);
    }

    private static final int ACCENT = Color.rgb(255, 155, 69), SURFACE = Color.rgb(28, 30, 33);
    private static final int TEXT = Color.rgb(244, 240, 235), MUTED = Color.rgb(168, 162, 156);

    private final Listener listener;
    private final float density;
    private final LinearLayout card;
    private final TextView heading, detail, counter;
    private final EditText field;
    private int maxLength;
    private boolean showing;

    TextPrompt(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        density = getResources().getDisplayMetrics().density;
        setVisibility(GONE);
        setBackgroundColor(Color.argb(160, 0, 0, 0));
        setClickable(true);  // the dimmed game underneath takes no touches while this is open
        setFocusable(true);

        card = new LinearLayout(context);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(14), dp(18), dp(14));
        GradientDrawable background = new GradientDrawable();
        background.setColor(SURFACE);
        background.setCornerRadius(dp(18));
        background.setStroke(dp(2), ACCENT);
        card.setBackground(background);
        card.setElevation(dp(8));
        card.setClickable(true);  // taps on the card must not reach the scrim

        heading = new TextView(context);
        heading.setTextColor(TEXT);
        heading.setTextSize(17);
        heading.setTypeface(null, Typeface.BOLD);
        card.addView(heading);

        detail = new TextView(context);
        detail.setTextColor(MUTED);
        detail.setTextSize(13);
        card.addView(detail);

        field = new EditText(context);
        field.setTextColor(TEXT);
        field.setHintTextColor(MUTED);
        field.setTextSize(18);
        field.setSingleLine(true);
        field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        // A landscape keyboard would otherwise take the whole screen with its own editor.
        field.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        field.setBackgroundTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        field.setOnEditorActionListener((v, action, event) -> {
            if (action == EditorInfo.IME_ACTION_DONE) { accept(); return true; }
            return false;
        });
        LinearLayout.LayoutParams fieldAt = new LinearLayout.LayoutParams(-1, -2);
        fieldAt.topMargin = dp(8);
        card.addView(field, fieldAt);

        counter = new TextView(context);
        counter.setTextColor(MUTED);
        counter.setTextSize(12);
        counter.setGravity(Gravity.END);
        card.addView(counter, new LinearLayout.LayoutParams(-1, -2));
        field.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(android.text.Editable s) { updateCounter(); }
        });

        LinearLayout buttons = new LinearLayout(context);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.END);
        Button cancel = button("Cancelar", false);
        cancel.setOnClickListener(v -> cancel());
        Button ok = button("OK", true);
        ok.setOnClickListener(v -> accept());
        LinearLayout.LayoutParams buttonAt = new LinearLayout.LayoutParams(-2, dp(44));
        buttonAt.leftMargin = dp(10);
        buttons.addView(cancel, buttonAt);
        buttons.addView(ok, buttonAt);
        LinearLayout.LayoutParams buttonsAt = new LinearLayout.LayoutParams(-1, -2);
        buttonsAt.topMargin = dp(6);
        card.addView(buttons, buttonsAt);

        int width = Math.min(dp(520), getResources().getDisplayMetrics().widthPixels - dp(32));
        LayoutParams cardAt = new LayoutParams(width, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        cardAt.topMargin = dp(10);
        addView(card, cardAt);
    }

    boolean isShowing() { return showing; }

    /** Opens the box; a request that arrives while one is open replaces it (and cancels it). */
    void show(String title, String description, String initial, int maxLength) {
        if (showing) answer("", true);
        this.maxLength = Math.max(1, maxLength);
        heading.setText(title == null || title.isEmpty() ? "Digite o texto" : title);
        detail.setText(description == null ? "" : description);
        detail.setVisibility(description == null || description.isEmpty() ? GONE : VISIBLE);
        field.setFilters(new InputFilter[] { new InputFilter.LengthFilter(this.maxLength) });
        field.setText(initial == null ? "" : initial);
        field.selectAll();  // typing replaces the suggested text, as the console's keyboard does
        updateCounter();
        showing = true;
        setVisibility(VISIBLE);
        bringToFront();
        field.requestFocus();
        // The keyboard needs the view laid out and the window focused before it will rise.
        field.post(() -> {
            field.requestFocus();
            InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.showSoftInput(field, InputMethodManager.SHOW_IMPLICIT);
        });
    }

    void cancel() { answer("", true); }

    private void accept() { answer(field.getText().toString(), false); }

    private void answer(String text, boolean cancelled) {
        if (!showing) return;
        showing = false;
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) imm.hideSoftInputFromWindow(field.getWindowToken(), 0);
        field.clearFocus();
        setVisibility(GONE);
        listener.onAnswer(text, cancelled);
    }

    private void updateCounter() {
        counter.setText(field.getText().length() + " / " + maxLength);
    }

    private Button button(String label, boolean filled) {
        Button button = new Button(getContext());
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(15);
        button.setTypeface(null, Typeface.BOLD);
        button.setMinWidth(dp(88));
        button.setMinimumWidth(dp(88));
        button.setPadding(dp(16), 0, dp(16), 0);
        GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(dp(12));
        if (filled) {
            shape.setColor(ACCENT);
            button.setTextColor(Color.rgb(16, 17, 19));
        } else {
            shape.setColor(Color.TRANSPARENT);
            shape.setStroke(dp(1), MUTED);
            button.setTextColor(TEXT);
        }
        button.setBackground(shape);
        return button;
    }

    private int dp(int value) { return Math.round(value * density); }
}
