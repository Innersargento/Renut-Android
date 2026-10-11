package org.renut.android;

import android.content.Intent;
import android.os.Bundle;
import android.view.View;

/**
 * The screen layout: the list of saved layouts (no extra) or the editor of one of them
 * (EXTRA_PROFILE). Opened from Configuracoes and from the in-game sidebar; from the game it covers
 * the game, which stays alive behind it as when the app is left. The screens are Compose.
 */
public final class LayoutEditorActivity extends androidx.activity.ComponentActivity {
    static final String EXTRA_PROFILE = "profile";
    private LayoutListModel list;
    private LayoutEditorModel editor;

    @Override protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(Language.attach(base));
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Transitions.slideOpen(this);
        String profile = getIntent().getStringExtra(EXTRA_PROFILE);
        if (HudLayout.DEFAULT.equals(profile)) {
            finish();  // the built-in layout is not editable; "create" starts from it
            return;
        }
        if (profile == null) {
            // The list of layouts is an ordinary screen: it follows the phone's orientation, portrait included.
            list = new LayoutListModel(this);
            ComposeScreens.showLayoutList(this, list);
        } else {
            // The editor shows the layout as the game screen has it: landscape, full screen.
            setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            if (android.os.Build.VERSION.SDK_INT >= 28)
                getWindow().getAttributes().layoutInDisplayCutoutMode =
                    android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            editor = new LayoutEditorModel(this, profile);
            ComposeScreens.showLayoutEditor(this, editor);
        }
    }

    @Override protected void onResume() {
        super.onResume();
        if (list != null) list.refresh();  // back from an editor: its save changed the list
    }

    /** Opens the editor of one saved layout (the list stays underneath). */
    public void edit(String name) {
        if (HudLayout.DEFAULT.equals(name)) return;
        startActivity(new Intent(this, LayoutEditorActivity.class).putExtra(EXTRA_PROFILE, name));
    }

    /** Done and Back in the editor: asks about unsaved changes first. */
    public void leave() {
        if (editor != null && editor.getDirty()) editor.setAsking(true);
        else finish();
    }

    @Override public void onBackPressed() { leave(); }

    @Override public void finish() {
        super.finish();
        Transitions.slideClose(this);
    }
}
