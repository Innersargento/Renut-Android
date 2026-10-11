package org.renut.android;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.util.ArrayList;

/** Lets the user keep the Android standard mapping or capture each control. */
public final class ControllerSettingsActivity extends androidx.activity.ComponentActivity {
    private final ArrayList<InputDevice> devices = new ArrayList<>();
    private Spinner chooser;
    private LinearLayout rows;
    private TextView status;
    private ExternalController.Profile profile;
    private InputDevice device;
    private int pending = -1;
    private int ink = Color.rgb(226, 237, 235);

    @Override public void finish() {
        super.finish();
        Transitions.slideClose(this);
    }

    @Override protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(Language.attach(base));
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Transitions.slideOpen(this);
        getWindow().setStatusBarColor(Color.rgb(12, 23, 28));
        getWindow().setNavigationBarColor(Color.rgb(12, 23, 28));
        ScrollView scroll = new ScrollView(this);
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        body.setPadding(pad, pad, pad, pad);
        body.setBackgroundColor(Color.rgb(12, 23, 28));
        scroll.addView(body);
        setContentView(scroll);

        TextView title = label("CONTROLE EXTERNO", 24); body.addView(title);
        body.addView(label(Language.s(R.string.ctrl_intro), 14));
        chooser = new Spinner(this); body.addView(chooser);
        button(body, Language.s(R.string.ctrl_refresh), this::refreshDevices);
        button(body, Language.s(R.string.ctrl_auto), () -> {
            if (device == null) return;
            ExternalController.reset(this, device);
            profile = ExternalController.automatic(device);
            pending = -1;
            status.setText(Language.s(R.string.ctrl_status_default));
            renderRows();
        });
        status = label("", 15); body.addView(status);
        button(body, Language.s(R.string.ctrl_cancel_capture), () -> {
            pending = -1;
            status.setText(Language.s(R.string.ctrl_status_select));
        });
        rows = new LinearLayout(this); rows.setOrientation(LinearLayout.VERTICAL); body.addView(rows);
        refreshDevices();
        ComposeScreens.showController(this);
    }

    public java.util.List<String> uiDevices(){
        ArrayList<String> names=new ArrayList<>();for(InputDevice item:devices)names.add(item.getName());return names;
    }
    public int uiSelected(){return chooser.getSelectedItemPosition();}
    public void uiSelect(int index){if(index>=0&&index<devices.size())chooser.setSelection(index);}
    public String uiStatus(){return status.getText().toString();}
    public java.util.List<String> uiBindings(){
        ArrayList<String> labels=new ArrayList<>();
        if(profile!=null)for(int i=0;i<ExternalController.COUNT;i++)labels.add(ExternalController.label(i)+"  ·  "+profile.describe(i));
        return labels;
    }
    public int uiPending(){return pending;}
    public void uiRefresh(){refreshDevices();}
    public void uiAutomatic(){if(device!=null){ExternalController.reset(this,device);profile=ExternalController.automatic(device);pending=-1;status.setText(Language.s(R.string.ctrl_status_default));renderRows();}}
    public void uiCancel(){pending=-1;status.setText(Language.s(R.string.ctrl_status_select));}
    public void uiCapture(int index){if(index>=0&&index<rows.getChildCount())rows.getChildAt(index).performClick();}

    @Override protected void onResume() { super.onResume(); if (chooser != null) refreshDevices(); }

    private int dp(int value) { return (int)(value * getResources().getDisplayMetrics().density); }
    private TextView label(String value, int size) {
        TextView text = new TextView(this);
        text.setText(value); text.setTextColor(ink); text.setTextSize(size);
        text.setPadding(0, dp(8), 0, dp(8));
        return text;
    }
    private void button(LinearLayout parent, String title, Runnable action) {
        Button button = new Button(this); button.setText(title); button.setAllCaps(false);
        button.setOnClickListener(view -> action.run()); parent.addView(button);
    }

    private void refreshDevices() {
        devices.clear();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice found = InputDevice.getDevice(id);
            if (ExternalController.isGamepad(found)) devices.add(found);
        }
        ArrayList<String> names = new ArrayList<>();
        for (InputDevice found : devices) names.add(found.getName());
        if (names.isEmpty()) names.add(Language.s(R.string.ctrl_none));
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, names) {
            @Override public View getView(int position, View convertView, android.view.ViewGroup parent) {
                View view = super.getView(position, convertView, parent);
                ((TextView)view).setTextColor(ink);
                return view;
            }
        };
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        chooser.setAdapter(adapter);
        String selected = ExternalController.selected(this);
        int index = 0;
        for (int i = 0; i < devices.size(); i++) {
            if (ExternalController.descriptor(devices.get(i)).equals(selected)) { index = i; break; }
        }
        if (!devices.isEmpty()) chooser.setSelection(index);
        chooser.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view, int position, long id) {
                device = position < devices.size() ? devices.get(position) : null;
                pending = -1;
                if (device != null) {
                    ExternalController.select(ControllerSettingsActivity.this, device);
                    profile = ExternalController.load(ControllerSettingsActivity.this, device);
                    status.setText(Language.s(R.string.ctrl_status_loaded));
                } else {
                    profile = null;
                    status.setText(Language.s(R.string.ctrl_status_connect));
                }
                renderRows();
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        if (devices.isEmpty()) { device = null; profile = null; rows.removeAllViews();
            status.setText(Language.s(R.string.ctrl_status_connect)); }
    }

    private void renderRows() {
        rows.removeAllViews();
        if (profile == null) return;
        for (int i = 0; i < ExternalController.COUNT; i++) {
            final int index = i;
            button(rows, ExternalController.label(i) + "  ·  " + profile.describe(i), () -> {
                pending = index;
                status.setText(index >= ExternalController.AXIS_START
                    ? Language.s(R.string.ctrl_move, ExternalController.label(index))
                    : Language.s(R.string.ctrl_press, ExternalController.label(index)));
            });
        }
    }

    private boolean captureDevice(InputDevice eventDevice) {
        return pending >= 0 && device != null && ExternalController.isGamepad(eventDevice)
            && ExternalController.descriptor(device).equals(ExternalController.descriptor(eventDevice));
    }

    private void complete(ExternalController.Binding binding) {
        int index = pending;
        profile.bindings[index] = binding;
        ExternalController.save(this, device, profile);
        pending = -1;
        status.setText(Language.s(R.string.ctrl_assigned, ExternalController.label(index), binding.label()));
        renderRows();
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (captureDevice(event.getDevice()) && event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            if (pending >= ExternalController.AXIS_START) {
                status.setText(Language.s(R.string.ctrl_need_axis));
                return true;
            }
            complete(new ExternalController.Binding(true, event.getKeyCode(), 1));
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (captureDevice(event.getDevice())
            && (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            InputDevice.MotionRange best = null;
            float movement = 0, direction = 0;
            for (InputDevice.MotionRange range : device.getMotionRanges()) {
                if ((range.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) continue;
                float raw = event.getAxisValue(range.getAxis());
                float neutral = range.getMin() >= 0 ? range.getMin() : 0;
                float span = Math.max(0.01f, range.getMax() - range.getMin());
                float amount = Math.abs(raw - neutral) / span;
                if (amount > movement) { best = range; movement = amount; direction = raw - neutral; }
            }
            if (best != null && movement >= 0.3f) {
                complete(new ExternalController.Binding(false, best.getAxis(), direction < 0 ? -1 : 1));
                return true;
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }
}
