package org.renut.android;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.view.*;
import android.widget.*;
import java.io.File;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;

/** Isolates the Vulkan loader and the guest runtime for each validation session. */
public final class RuntimeActivity extends androidx.activity.ComponentActivity implements SurfaceHolder.Callback {
    private TextView status;
    private SurfaceView surface;
    private GamepadOverlay overlay;
    private View gameMenu;
    private boolean controlsVisible;
    private float gameRefreshRate=60f;
    private ExternalController.Player controller;
    private int controllerDeviceId = -1;
    private InputManager inputManager;
    private final InputManager.InputDeviceListener deviceListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { if (launch) setupController(); }
        @Override public void onInputDeviceChanged(int id) { if (launch && id == controllerDeviceId) { clearController(); setupController(); } }
        @Override public void onInputDeviceRemoved(int id) { if (id == controllerDeviceId) { clearController(); setupController(); } }
    };
    private ScrollView results;
    private boolean started, graphics, launch;
    private volatile boolean loaded, cancelled;
    private native String nativeValidate(String filesDirectory,Surface surface,String hooks,String directory,String library,boolean launch,boolean oneUiFix,boolean sceneMsaa,String tuDebug,String ir3Debug);
    private static native void nativeStop();
    private static native void nativeBackground();
    private static native boolean nativeForeground(Surface surface);
    private static native void nativeButton(int button,boolean down);
    private static native void nativeStick(int stick,float x,float y);
    private static native void nativeTrigger(int trigger,float value);
    private static native void nativeExternalButton(int button,boolean down);
    private static native void nativeExternalStick(int stick,float x,float y);
    private static native void nativeExternalTrigger(int trigger,float value);
    private static native long nativeFrameCount();
    private TextView fps;
    private AchievementToast achievementToast;
    // The one game screen that is open: the native side reports unlocks through a static method.
    private static volatile java.lang.ref.WeakReference<RuntimeActivity> current;
    /** Called from the guest thread that wrote the achievement, once it is saved. */
    public static void onAchievementUnlocked(int id){
        RuntimeActivity activity=current==null?null:current.get();
        if(activity==null)return;
        activity.runOnUiThread(()->{
            if(activity.isFinishing()||activity.achievementToast==null)return;
            Achievements.Item item=Achievements.find(activity.getFilesDir(),id);
            if(item!=null)activity.achievementToast.push(item);
        });
    }
    public int openContentFd(String uri, String mode) throws java.io.IOException {
        android.os.ParcelFileDescriptor descriptor=getContentResolver().openFileDescriptor(android.net.Uri.parse(uri),mode);
        if(descriptor==null)throw new java.io.IOException("Arquivo indisponível.");
        try {return descriptor.detachFd();} finally {descriptor.close();}
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        launch=getIntent().getBooleanExtra("launch",false);
        if(launch && "true".equals(RuntimeOptions.get(this,"renut_plume_uncap_fps","false"))){
            Display display=getDisplay();
            if(display!=null){
                Display.Mode current=display.getMode();
                for(Display.Mode mode:display.getSupportedModes()){
                    if(mode.getPhysicalWidth()==current.getPhysicalWidth() &&
                       mode.getPhysicalHeight()==current.getPhysicalHeight())
                        gameRefreshRate=Math.max(gameRefreshRate,mode.getRefreshRate());
                }
            }
        }
        if(launch)setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        graphics=launch||getIntent().getBooleanExtra("graphics",false);
        if(launch){
            inputManager=(InputManager)getSystemService(INPUT_SERVICE);
            inputManager.registerInputDeviceListener(deviceListener,null);
            setupController();
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if(launch){
            // The notification is how the user gets back to a game left in the background;
            // without the permission (Android 13+) the game still stays alive, just unlisted.
            if(android.os.Build.VERSION.SDK_INT>=33&&checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)!=android.content.pm.PackageManager.PERMISSION_GRANTED)
                requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS},7);
            KeepAliveService.start(this);
        }
        status=new TextView(this);status.setTextColor(Color.rgb(226,237,235));status.setTextSize(16);
        results=new ScrollView(this);results.addView(status);
        if(launch){
            // Full screen game surface with optional on-screen controls.
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                |View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                |View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            if(android.os.Build.VERSION.SDK_INT>=28)getWindow().getAttributes().layoutInDisplayCutoutMode=
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            WindowManager.LayoutParams windowParams=getWindow().getAttributes();
            windowParams.preferredRefreshRate=gameRefreshRate;
            getWindow().setAttributes(windowParams);
            FrameLayout root=new FrameLayout(this);root.setBackgroundColor(Color.BLACK);
            surface=new SurfaceView(this);surface.getHolder().addCallback(this);
            root.addView(surface,new FrameLayout.LayoutParams(-1,-1));
            overlay=new GamepadOverlay(this,new GamepadOverlay.Sink(){
                @Override public void button(int mask,boolean down){if(loaded)nativeButton(mask,down);}
                @Override public void stick(int stick,float x,float y){if(loaded)nativeStick(stick,x,y);}
                @Override public void trigger(int trigger,float value){if(loaded)nativeTrigger(trigger,value);}
                @Override public void exit(){showGameMenu();}
            });
            controlsVisible=getIntent().getBooleanExtra("showControls",true);
            overlay.setVisibility(controlsVisible?View.VISIBLE:View.GONE);
            root.addView(overlay,new FrameLayout.LayoutParams(-1,-1));
            if(getIntent().getBooleanExtra("fps",false)){
                // Frames the game presented (D3DDevice_Swap) per second, sampled every 0.5 s.
                fps=new TextView(this);fps.setTextColor(Color.rgb(120,255,120));fps.setTextSize(14);
                fps.setTypeface(android.graphics.Typeface.MONOSPACE,android.graphics.Typeface.BOLD);
                fps.setShadowLayer(3,1,1,Color.BLACK);fps.setText("FPS --");
                FrameLayout.LayoutParams at=new FrameLayout.LayoutParams(-2,-2,Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL);
                at.bottomMargin=(int)(8*getResources().getDisplayMetrics().density);
                root.addView(fps,at);
                final long[] last={-1,0};
                fps.postDelayed(new Runnable(){@Override public void run(){
                    if(isFinishing())return;
                    if(loaded){
                        long frames=nativeFrameCount(),now=android.os.SystemClock.elapsedRealtime();
                        if(last[0]>=0&&now>last[1])fps.setText(String.format(java.util.Locale.ROOT,"FPS %.1f",(frames-last[0])*1000.0/(now-last[1])));
                        last[0]=frames;last[1]=now;
                    }
                    fps.postDelayed(this,500);
                }},500);
            }
            results.setVisibility(View.GONE);results.setBackgroundColor(Color.rgb(12,23,28));
            root.addView(results,new FrameLayout.LayoutParams(-1,-1));
            gameMenu=ComposeScreens.createGameMenu(this);
            gameMenu.setVisibility(View.GONE);
            root.addView(gameMenu,new FrameLayout.LayoutParams(-1,-1));
            achievementToast=new AchievementToast(this);
            root.addView(achievementToast,AchievementToast.params(this));
            current=new java.lang.ref.WeakReference<>(this);
            setContentView(root);
            return;
        }
        LinearLayout body=new LinearLayout(this);body.setOrientation(LinearLayout.VERTICAL);
        int pad=(int)(20*getResources().getDisplayMetrics().density);
        body.setPadding(pad,pad*2,pad,pad);body.setBackgroundColor(Color.rgb(12,23,28));
        body.setOnApplyWindowInsetsListener((v,i)->{v.setPadding(pad,pad+i.getSystemWindowInsetTop(),pad,pad+i.getSystemWindowInsetBottom());return i;});
        status.setText(graphics?"RexGlue + Plume\nImagem de teste na memória do runtime. Ainda não representa cenas do jogo.":"Validando o runtime ARM64…\nEste teste ainda não inicia o jogo.");
        body.addView(results,new LinearLayout.LayoutParams(-1,graphics?-2:0,graphics?0:1));
        if(graphics){surface=new SurfaceView(this);surface.getHolder().addCallback(this);body.addView(surface,new LinearLayout.LayoutParams(-1,0,1));}
        Button close=new Button(this);close.setText("Voltar");close.setOnClickListener(v->finish());body.addView(close);
        ComposeScreens.showRuntime(this);
        if(!graphics)startValidation(null);
    }
    public boolean uiGraphics(){return graphics;}
    public String uiStatus(){return status.getText().toString();}
    public void attachComposeSurface(SurfaceView view){surface=view;surface.getHolder().addCallback(this);}
    public boolean uiGameControlsVisible(){return controlsVisible;}
    public void uiSetGameControlsVisible(boolean visible){
        controlsVisible=visible;
        if(!visible)overlay.cancelTouches();
        overlay.setVisibility(visible?View.VISIBLE:View.GONE);
        getSharedPreferences("MainActivity",MODE_PRIVATE).edit().putBoolean("showControls",visible).apply();
    }
    public void uiCloseGameMenu(){gameMenu.setVisibility(View.GONE);}
    private void showGameMenu(){
        if(!launch||gameMenu==null||results.getVisibility()==View.VISIBLE)return;
        overlay.cancelTouches();
        gameMenu.setVisibility(View.VISIBLE);
    }
    @Override public void onBackPressed(){
        if(launch&&gameMenu!=null&&gameMenu.getVisibility()==View.VISIBLE){gameMenu.setVisibility(View.GONE);return;}
        if(launch&&results.getVisibility()!=View.VISIBLE){showGameMenu();return;}
        super.onBackPressed();
    }
    private void setupController(){
        if(controller!=null)return;
        InputDevice device=ExternalController.findSelected(this);
        if(device==null)return;
        controllerDeviceId=device.getId();
        controller=new ExternalController.Player(device,ExternalController.load(this,device),new ExternalController.Sink(){
            @Override public void button(int mask,boolean down){if(loaded)nativeExternalButton(mask,down);}
            @Override public void stick(int stick,float x,float y){if(loaded)nativeExternalStick(stick,x,y);}
            @Override public void trigger(int trigger,float value){if(loaded)nativeExternalTrigger(trigger,value);}
        });
    }
    private void clearController(){
        if(controller!=null)controller.release();
        controller=null;controllerDeviceId=-1;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        if(launch&&event.getKeyCode()==KeyEvent.KEYCODE_BACK){
            if(event.getAction()==KeyEvent.ACTION_UP)onBackPressed();
            return true;
        }
        if(launch&&gameMenu!=null&&gameMenu.getVisibility()==View.VISIBLE)return super.dispatchKeyEvent(event);
        if(launch){setupController();if(controller!=null&&controller.key(event))return true;}
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event){
        if(launch){setupController();if(controller!=null&&controller.motion(event))return true;}
        return super.dispatchGenericMotionEvent(event);
    }
    private void startValidation(Surface target){
        if(started||cancelled)return;started=true;
        new Thread(()->{
            String report;
            try {
                System.loadLibrary("SDL3");org.libsdl.app.SDL.setupJNI();org.libsdl.app.SDL.initialize();org.libsdl.app.SDL.setContext(this);
                System.loadLibrary("renut_runtime");loaded=true;
                if(cancelled){nativeStop();return;}
                report=nativeValidate(getFilesDir().getAbsolutePath(),target,getApplicationInfo().nativeLibraryDir,
                    getIntent().getStringExtra("driverDir"),getIntent().getStringExtra("driverLib"),
                    launch,getIntent().getBooleanExtra("oneUiFix",true),
                    getIntent().getBooleanExtra("sceneMsaa",false),
                    getIntent().getStringExtra("tuDebug"),getIntent().getStringExtra("ir3Debug"));
            }catch(Throwable e){
                report="Falha ao iniciar o runtime: "+e;
                try{Files.write(new File(getFilesDir(),"runtime-report.txt").toPath(),report.getBytes(StandardCharsets.UTF_8));}catch(Exception ignored){}
            }
            final String message=report;
            runOnUiThread(()->{if(!isFinishing()){status.setText(message);if(surface!=null)surface.setVisibility(View.GONE);
                if(launch){if(overlay!=null)overlay.setVisibility(View.GONE);if(fps!=null)fps.setVisibility(View.GONE);results.setVisibility(View.VISIBLE);}
                else results.setLayoutParams(new LinearLayout.LayoutParams(-1,0,1));}});
        },"renut-runtime-validation").start();
    }
    private volatile boolean inBackground;
    private void stop(){cancelled=true;if(loaded)nativeStop();}
    @Override public void surfaceCreated(SurfaceHolder h){
        // Request the selected display rate for the game surface as well as
        // the window; the default 60 Hz keeps an even cadence for 30 FPS.
        if(launch&&android.os.Build.VERSION.SDK_INT>=30){
            try{h.getSurface().setFrameRate(gameRefreshRate,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);}catch(Exception ignored){}
        }
        if(launch&&started&&loaded&&inBackground){
            // Back on screen with a new surface: the game was kept alive, give it the window.
            if(nativeForeground(h.getSurface()))inBackground=false;
            return;
        }
        startValidation(h.getSurface());
    }
    @Override public void surfaceChanged(SurfaceHolder h,int format,int width,int height){}
    @Override public void surfaceDestroyed(SurfaceHolder h){
        // The game is kept, paused, while the app is away (see KeepAliveService). Only the
        // diagnostic screens, which own no game state, stop with their surface.
        if(launch&&started&&loaded&&!isFinishing()){inBackground=true;nativeBackground();}
        else{stop();if(launch&&!isFinishing())finish();}  // not loaded yet: nothing to keep
    }
    // Leaving the screen no longer ends the game; only closing it does (onDestroy).
    @Override protected void onPause(){clearController();super.onPause();if(!launch){stop();finish();}}
    @Override protected void onResume(){super.onResume();if(launch)setupController();}
    @Override protected void onDestroy(){if(inputManager!=null)inputManager.unregisterInputDeviceListener(deviceListener);stop();if(launch)KeepAliveService.stop(this);super.onDestroy();if(isFinishing())android.os.Process.killProcess(android.os.Process.myPid());}
}
