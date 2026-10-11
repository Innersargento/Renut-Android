package org.renut.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen Xbox 360 controller drawn over the game. Layout follows the pad:
 * triggers and bumpers on top, left stick high and D-pad low on the left, face
 * buttons high and right stick low on the right, Back/Guide/Start centred.
 * Every control is multitouch; a finger can slide across the face buttons and
 * the D-pad. A quick tap on a stick clicks it (L3/R3). Holding Guide exits.
 */
final class GamepadOverlay extends View {
    interface Sink {
        void button(int mask, boolean down);
        void stick(int stick, float x, float y);
        void trigger(int trigger, float value);
        void exit();
    }

    // XInput button masks.
    static final int UP = 0x1, DOWN = 0x2, LEFT = 0x4, RIGHT = 0x8, START = 0x10, BACK = 0x20,
            L3 = 0x40, R3 = 0x80, LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;

    private static final int K_BUTTON = 0, K_STICK = 1, K_DPAD = 2, K_TRIGGER = 3;
    private static final float DEAD_ZONE = 0.12f;
    private static final long CLICK_MS = 180, CLICK_PULSE_MS = 150;

    private static final class Control {
        final int kind, mask, index;
        final String label;
        final String id;      // the element's name in HudLayout
        final int color;
        float cx, cy, r;      // circles
        final RectF rect = new RectF(); // triggers and bumpers
        boolean pressed;
        boolean hidden;       // the profile leaves it out: neither drawn nor touchable
        float opacity = 1f;   // the profile's, for this control alone
        float valueX, valueY; // stick deflection or trigger value
        Control(String id, int kind, int mask, int index, String label, int color) {
            this.id = id; this.kind = kind; this.mask = mask; this.index = index; this.label = label; this.color = color;
        }
        boolean hit(float x, float y, float slop) {
            if (kind == K_TRIGGER || (kind == K_BUTTON && !rect.isEmpty())) {
                return x >= rect.left - slop && x <= rect.right + slop && y >= rect.top - slop && y <= rect.bottom + slop;
            }
            float dx = x - cx, dy = y - cy, reach = r + slop;
            return dx * dx + dy * dy <= reach * reach;
        }
    }

    private static final class Touch {
        Control control;
        float startX, startY;
        long downAt;
        boolean moved;
    }

    private final Sink sink;
    private final Control[] controls;
    private final Control leftStick, rightStick, dpad;
    private final SparseArray<Touch> touches = new SparseArray<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int dpadMask;

    GamepadOverlay(Context context, Sink sink) {
        super(context);
        this.sink = sink;
        leftStick = new Control(HudLayout.STICK_L, K_STICK, L3, 0, "", Color.WHITE);
        rightStick = new Control(HudLayout.STICK_R, K_STICK, R3, 1, "", Color.WHITE);
        dpad = new Control(HudLayout.DPAD, K_DPAD, 0, 0, "", Color.WHITE);
        controls = new Control[] {
            new Control(HudLayout.LT, K_TRIGGER, 0, 0, "LT", Color.WHITE),
            new Control(HudLayout.RT, K_TRIGGER, 0, 1, "RT", Color.WHITE),
            new Control(HudLayout.LB, K_BUTTON, LB, 0, "LB", Color.WHITE),
            new Control(HudLayout.RB, K_BUTTON, RB, 0, "RB", Color.WHITE),
            new Control(HudLayout.BACK, K_BUTTON, BACK, 0, "BACK", Color.WHITE),
            new Control(HudLayout.START, K_BUTTON, START, 0, "START", Color.WHITE),
            new Control(HudLayout.A, K_BUTTON, A, 0, "A", Color.rgb(93, 187, 47)),
            new Control(HudLayout.B, K_BUTTON, B, 0, "B", Color.rgb(216, 39, 42)),
            new Control(HudLayout.X, K_BUTTON, X, 0, "X", Color.rgb(30, 115, 190)),
            new Control(HudLayout.Y, K_BUTTON, Y, 0, "Y", Color.rgb(245, 184, 0)),
            leftStick, rightStick, dpad,
        };
        line.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setClickable(true);
    }

    private HudLayout.Profile profile;

    /** The profile that says where each control sits, how big it is and whether it is shown. */
    void setLayout(HudLayout.Profile layout) {
        cancelTouches();
        profile = layout;
        applyLayout(getWidth(), getHeight());
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        applyLayout(w, h);
    }

    private void applyLayout(int w, int h) {
        if (w <= 0 || h <= 0) return;
        float u = Math.min(w, h) / 10f; // layout unit
        for (Control c : controls) {
            HudLayout.Shape shape = HudLayout.shape(profile, c.id, w, h);
            c.hidden = !HudLayout.visible(profile, c.id);
            c.opacity = HudLayout.opacity(profile, c.id);
            if (shape.circle) {
                place(c, shape.cx, shape.cy, shape.r);
            } else {
                c.rect.set(shape.cx - shape.hw, shape.cy - shape.hh, shape.cx + shape.hw, shape.cy + shape.hh);
            }
        }
        text.setTextSize(u * 0.42f);
        line.setStrokeWidth(Math.max(2f, u * 0.05f));
    }

    private static void place(Control c, float x, float y, float r) {
        c.cx = x; c.cy = y; c.r = r; c.rect.setEmpty();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        for (Control c : controls) {
            if (c.hidden) continue;
            // One layer per control that is not fully opaque: everything it draws is faded together.
            final boolean faded = c.opacity < 0.999f;
            if (faded) {
                float pad = c.kind == K_BUTTON && !c.rect.isEmpty() || c.kind == K_TRIGGER ? 4f : c.r + 4f;
                RectF bounds = c.rect.isEmpty() ? new RectF(c.cx - pad, c.cy - pad, c.cx + pad, c.cy + pad)
                        : new RectF(c.rect.left - 4, c.rect.top - 4, c.rect.right + 4, c.rect.bottom + 4);
                canvas.saveLayerAlpha(bounds, Math.round(255 * c.opacity));
            }
            int alpha = c.pressed ? 150 : 70;
            fill.setColor(c.color); fill.setAlpha(alpha);
            line.setColor(Color.WHITE); line.setAlpha(c.pressed ? 220 : 130);
            text.setColor(Color.WHITE); text.setAlpha(c.pressed ? 255 : 200);
            float textY = -(text.descent() + text.ascent()) / 2f;
            switch (c.kind) {
                case K_TRIGGER: {
                    fill.setColor(Color.WHITE); fill.setAlpha(50);
                    canvas.drawRoundRect(c.rect, c.rect.height() / 3, c.rect.height() / 3, fill);
                    if (c.valueX > 0) {
                        RectF level = new RectF(c.rect);
                        level.top = c.rect.bottom - c.rect.height() * c.valueX;
                        fill.setAlpha(140);
                        canvas.drawRoundRect(level, c.rect.height() / 3, c.rect.height() / 3, fill);
                    }
                    canvas.drawRoundRect(c.rect, c.rect.height() / 3, c.rect.height() / 3, line);
                    canvas.drawText(c.label, c.rect.centerX(), c.rect.centerY() + textY, text);
                    break;
                }
                case K_BUTTON:
                    if (!c.rect.isEmpty()) {
                        fill.setColor(Color.WHITE); fill.setAlpha(alpha);
                        canvas.drawRoundRect(c.rect, c.rect.height() / 2, c.rect.height() / 2, fill);
                        canvas.drawRoundRect(c.rect, c.rect.height() / 2, c.rect.height() / 2, line);
                        canvas.drawText(c.label, c.rect.centerX(), c.rect.centerY() + textY, text);
                    } else {
                        canvas.drawCircle(c.cx, c.cy, c.r, fill);
                        canvas.drawCircle(c.cx, c.cy, c.r, line);
                        canvas.drawText(c.label, c.cx, c.cy + textY, text);
                    }
                    break;
                case K_STICK: {
                    fill.setColor(Color.WHITE); fill.setAlpha(40);
                    canvas.drawCircle(c.cx, c.cy, c.r, fill);
                    canvas.drawCircle(c.cx, c.cy, c.r, line);
                    fill.setAlpha(c.pressed ? 150 : 90);
                    float kx = c.cx + c.valueX * c.r * 0.6f, ky = c.cy - c.valueY * c.r * 0.6f;
                    canvas.drawCircle(kx, ky, c.r * 0.45f, fill);
                    canvas.drawCircle(kx, ky, c.r * 0.45f, line);
                    break;
                }
                case K_DPAD:
                    drawDpad(canvas, c);
                    break;
            }
            if (faded) canvas.restore();
        }
    }

    private void drawDpad(Canvas canvas, Control c) {
        float arm = c.r * 0.36f;
        String[] arrows = {"▲", "▼", "◀", "▶"};
        int[] masks = {UP, DOWN, LEFT, RIGHT};
        float[][] offsets = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        for (int i = 0; i < 4; ++i) {
            float x = c.cx + offsets[i][0] * c.r * 0.62f, y = c.cy + offsets[i][1] * c.r * 0.62f;
            RectF r = new RectF(x - arm, y - arm, x + arm, y + arm);
            boolean on = (dpadMask & masks[i]) != 0;
            fill.setColor(Color.WHITE); fill.setAlpha(on ? 150 : 60);
            canvas.drawRoundRect(r, arm * 0.3f, arm * 0.3f, fill);
            line.setAlpha(on ? 220 : 130);
            canvas.drawRoundRect(r, arm * 0.3f, arm * 0.3f, line);
            float textY = -(text.descent() + text.ascent()) / 2f;
            canvas.drawText(arrows[i], x, y + textY, text);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                Touch t = new Touch();
                t.startX = e.getX(i); t.startY = e.getY(i); t.downAt = SystemClock.uptimeMillis();
                t.control = controlAt(t.startX, t.startY, null);
                touches.put(e.getPointerId(i), t);
                if (t.control != null) press(t, t.startX, t.startY);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); ++i) {
                    Touch t = touches.get(e.getPointerId(i));
                    if (t != null) move(t, e.getX(i), e.getY(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (action == MotionEvent.ACTION_CANCEL) {
                    for (int i = 0; i < touches.size(); ++i) release(touches.valueAt(i), true);
                    touches.clear();
                } else {
                    int id = e.getPointerId(e.getActionIndex());
                    Touch t = touches.get(id);
                    if (t != null) release(t, false);
                    touches.remove(id);
                }
                break;
        }
        invalidate();
        return true;
    }

    void cancelTouches() {
        for (int i = 0; i < touches.size(); ++i) release(touches.valueAt(i), true);
        touches.clear();
        invalidate();
    }

    private Control controlAt(float x, float y, Control exclude) {
        float slop = Math.min(getWidth(), getHeight()) / 40f;
        Control best = null;
        for (Control c : controls) {
            if (c == exclude || c.hidden || !c.hit(x, y, slop)) continue;
            best = c;
            break;
        }
        return best;
    }

    private void press(Touch t, float x, float y) {
        Control c = t.control;
        switch (c.kind) {
            case K_BUTTON: c.pressed = true; sink.button(c.mask, true); break;
            case K_STICK: c.pressed = true; updateStick(c, x, y); break;
            case K_DPAD: updateDpad(x, y); break;
            case K_TRIGGER: c.pressed = true; updateTrigger(c, y); break;
        }
    }

    private void move(Touch t, float x, float y) {
        float travel = Math.min(getWidth(), getHeight()) / 30f;
        if (Math.abs(x - t.startX) > travel || Math.abs(y - t.startY) > travel) t.moved = true;
        Control c = t.control;
        if (c == null) return;
        switch (c.kind) {
            case K_STICK: updateStick(c, x, y); break;
            case K_DPAD: updateDpad(x, y); break;
            case K_TRIGGER: updateTrigger(c, y); break;
            case K_BUTTON:
                // Slide between face buttons (A/B/X/Y) like thumbing across a pad.
                if (!c.hit(x, y, 0)) {
                    Control next = controlAt(x, y, c);
                    if (next != null && next.kind == K_BUTTON && next.rect.isEmpty() && c.rect.isEmpty()) {
                        c.pressed = false; sink.button(c.mask, false);
                        t.control = next; next.pressed = true; sink.button(next.mask, true);
                    }
                }
                break;
            default:
                break;
        }
    }

    private void release(Touch t, boolean cancelled) {
        Control c = t.control;
        if (c == null) return;
        switch (c.kind) {
            case K_BUTTON: c.pressed = false; sink.button(c.mask, false); break;
            case K_STICK: {
                c.pressed = false; c.valueX = 0; c.valueY = 0;
                sink.stick(c.index, 0, 0);
                long held = SystemClock.uptimeMillis() - t.downAt;
                if (!cancelled && !t.moved && held < CLICK_MS) {
                    // Quick tap: click the stick (L3/R3) long enough for the game to sample it.
                    final int mask = c.mask;
                    sink.button(mask, true);
                    postDelayed(() -> sink.button(mask, false), CLICK_PULSE_MS);
                }
                break;
            }
            case K_DPAD: setDpad(0); break;
            case K_TRIGGER: c.pressed = false; c.valueX = 0; sink.trigger(c.index, 0); break;
        }
    }

    private void updateStick(Control c, float x, float y) {
        float dx = (x - c.cx) / c.r, dy = (c.cy - y) / c.r;
        float length = (float) Math.hypot(dx, dy);
        if (length > 1f) { dx /= length; dy /= length; length = 1f; }
        if (length < DEAD_ZONE) { dx = 0; dy = 0; }
        else {
            // Rescale past the dead zone so small movements still reach full range.
            float scale = (length - DEAD_ZONE) / (1f - DEAD_ZONE) / length;
            dx *= scale; dy *= scale;
        }
        c.valueX = dx; c.valueY = dy;
        sink.stick(c.index, dx, dy);
    }

    private void updateDpad(float x, float y) {
        float dx = x - dpad.cx, dy = y - dpad.cy;
        if (Math.hypot(dx, dy) < dpad.r * 0.2f) { setDpad(0); return; }
        // Eight sectors: diagonals press two directions.
        double angle = Math.toDegrees(Math.atan2(-dy, dx));
        if (angle < 0) angle += 360;
        int sector = (int) Math.round(angle / 45.0) % 8;
        int[] masks = {RIGHT, UP | RIGHT, UP, UP | LEFT, LEFT, DOWN | LEFT, DOWN, DOWN | RIGHT};
        setDpad(masks[sector]);
    }

    private void setDpad(int mask) {
        if (mask == dpadMask) return;
        int released = dpadMask & ~mask, pressedNow = mask & ~dpadMask;
        if (released != 0) sink.button(released, false);
        if (pressedNow != 0) sink.button(pressedNow, true);
        dpadMask = mask;
        dpad.pressed = mask != 0;
    }

    private void updateTrigger(Control c, float y) {
        // Touch triggers are full presses: a partial pull is hard to hold on glass.
        if (c.valueX == 1f) return;
        c.valueX = 1f;
        sink.trigger(c.index, 1f);
    }
}
