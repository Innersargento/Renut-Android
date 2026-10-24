package org.renut.android;

import android.graphics.Color;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.util.ArrayList;

/** Lets the user keep the Android standard mapping or capture each control. */
public final class ControllerSettingsActivity extends androidx.activity.ComponentActivity {
    private final ArrayList<InputDevice> devices = new ArrayList<>();
    private int selected;
    private String status = "";
    private ExternalController.Profile profile;
    private InputDevice device;
    private int pending = -1;

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
        getWindow().setStatusBarColor(Color.rgb(16,10,6));
        getWindow().setNavigationBarColor(Color.rgb(16,10,6));
        refreshDevices();
        ComposeScreens.showController(this);
    }

    public java.util.List<String> uiDevices(){
        ArrayList<String> names=new ArrayList<>();for(InputDevice item:devices)names.add(item.getName());return names;
    }
    public int uiSelected(){return selected;}
    public void uiSelect(int index){if(index>=0&&index<devices.size())selectDevice(index);}
    public String uiStatus(){return status;}
    public java.util.List<String> uiBindings(){
        ArrayList<String> labels=new ArrayList<>();
        if(profile!=null)for(int i=0;i<ExternalController.COUNT;i++)labels.add(ExternalController.label(i)+"  ·  "+profile.describe(i));
        return labels;
    }
    public int uiPending(){return pending;}
    public void uiRefresh(){refreshDevices();}
    public void uiAutomatic(){
        if (device == null) return;
        ExternalController.reset(this, device);
        profile = ExternalController.automatic(device);
        pending = -1;
        status = Language.s(R.string.ctrl_status_default);
    }
    public void uiCancel(){pending=-1;status=Language.s(R.string.ctrl_status_select);}
    /** Waits for the next press (or stick movement) to bind to control `index`. */
    public void uiCapture(int index){
        if (profile == null || index < 0 || index >= ExternalController.COUNT) return;
        pending = index;
        status = index >= ExternalController.AXIS_START
            ? Language.s(R.string.ctrl_move, ExternalController.label(index))
            : Language.s(R.string.ctrl_press, ExternalController.label(index));
    }

    @Override protected void onResume() { super.onResume(); refreshDevices(); }

    private void refreshDevices() {
        devices.clear();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice found = InputDevice.getDevice(id);
            if (ExternalController.isGamepad(found)) devices.add(found);
        }
        if (devices.isEmpty()) {
            device = null; profile = null; selected = 0; pending = -1;
            status = Language.s(R.string.ctrl_status_connect);
            return;
        }
        String chosen = ExternalController.selected(this);
        int index = 0;
        for (int i = 0; i < devices.size(); i++) {
            if (ExternalController.descriptor(devices.get(i)).equals(chosen)) { index = i; break; }
        }
        selectDevice(index);
    }

    private void selectDevice(int index) {
        selected = index;
        device = devices.get(index);
        pending = -1;
        ExternalController.select(this, device);
        profile = ExternalController.load(this, device);
        status = Language.s(R.string.ctrl_status_loaded);
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
        status = Language.s(R.string.ctrl_assigned, ExternalController.label(index), binding.label());
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (captureDevice(event.getDevice()) && event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            if (pending >= ExternalController.AXIS_START) {
                status = Language.s(R.string.ctrl_need_axis);
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
