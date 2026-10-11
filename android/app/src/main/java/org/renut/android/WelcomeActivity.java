package org.renut.android;

import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.WindowManager;
import java.io.InterruptedIOException;

/**
 * First run: a short introduction, the notice about what has to be imported, and the installation
 * of the game with a progress bar. It is shown while no game is imported; Configurações opens it
 * again (with {@link #EXTRA_IMPORT_ONLY}) to import another one, skipping the introduction.
 * The screens are Compose ({@link WelcomeModel}); this class owns the picker and the worker.
 */
public final class WelcomeActivity extends androidx.activity.ComponentActivity {
    public static final String EXTRA_IMPORT_ONLY = "importOnly";
    private static final int PICK_ISO = 20, PICK_XEX = 21;
    static final String PREFS = "MainActivity", SEEN = "welcomeSeen";

    private final WelcomeModel model = new WelcomeModel();
    private volatile Thread worker;
    private boolean importOnly;
    private long lastUpdate;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.rgb(16, 17, 19));
        getWindow().setNavigationBarColor(Color.rgb(16, 17, 19));
        importOnly = getIntent().getBooleanExtra(EXTRA_IMPORT_ONLY, false);
        boolean seen = getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(SEEN, false);
        model.setStep(importOnly || seen ? WelcomeModel.NOTICE : WelcomeModel.INTRO);
        ComposeScreens.showWelcome(this, model);
    }

    // Called by the screens ------------------------------------------------------------------

    public void uiContinueFromIntro() {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(SEEN, true).apply();
        model.setStep(WelcomeModel.NOTICE);
    }

    public void uiPick(boolean wholeDisc) {
        if (worker != null) return;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(intent, wholeDisc ? PICK_ISO : PICK_XEX);
    }

    public void uiCancelInstall() {
        Thread t = worker;
        if (t != null) t.interrupt();
    }

    public void uiBackToNotice() { model.setStep(WelcomeModel.NOTICE); }

    /** The game is installed: on to the launcher (or back to it, when this was a re-import). */
    public void uiFinish() {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(SEEN, true).apply();
        startActivity(new Intent(this, MainActivity.class).putExtra("noAutoStart", true)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP));
        finish();
    }

    // Import ---------------------------------------------------------------------------------

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if ((request != PICK_ISO && request != PICK_XEX) || result != RESULT_OK || data == null || data.getData() == null) return;
        if (worker != null) return;
        install(data.getData(), request == PICK_ISO);
    }

    private void install(Uri uri, boolean wholeDisc) {
        model.setWholeDisc(wholeDisc);
        model.setMessage("Preparando a importação…");
        model.setFraction(0f);
        model.setError("");
        model.setStep(WelcomeModel.INSTALL);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        worker = new Thread(() -> {
            String failure = null;
            boolean cancelled = false;
            try {
                GameImporter.run(this, uri, wholeDisc, (message, fraction) -> {
                    if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException("Importação cancelada.");
                    long now = SystemClock.elapsedRealtime();
                    if (now - lastUpdate < 200) return;
                    lastUpdate = now;
                    runOnUiThread(() -> { model.setMessage(message); model.setFraction((float) fraction); });
                });
            } catch (InterruptedIOException e) {
                cancelled = true;
            } catch (Exception e) {
                failure = e.getMessage() == null ? e.toString() : e.getMessage();
            }
            final boolean wasCancelled = cancelled;
            final String error = failure;
            runOnUiThread(() -> {
                worker = null;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                if (error != null) { model.setError(error); model.setStep(WelcomeModel.FAILED); }
                else if (wasCancelled) model.setStep(WelcomeModel.NOTICE);
                else { model.setFraction(1f); model.setStep(WelcomeModel.DONE); }
            });
        }, "renut-welcome-import");
        worker.start();
    }

    /** Back walks the steps backwards: notice -> introduction -> leaves the app. */
    @Override public void onBackPressed() {
        switch (model.getStep()) {
            case WelcomeModel.INSTALL: return;  // only Cancelar leaves an import
            case WelcomeModel.DONE: uiFinish(); return;
            case WelcomeModel.FAILED: model.setStep(WelcomeModel.NOTICE); return;
            case WelcomeModel.NOTICE:
                if (!importOnly) { model.setStep(WelcomeModel.INTRO); return; }  // re-import has no introduction
                break;
            default: break;
        }
        super.onBackPressed();
    }

    @Override protected void onDestroy() {
        Thread t = worker;
        if (t != null) t.interrupt();
        super.onDestroy();
    }
}
