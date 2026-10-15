package org.renut.android;

import android.app.Activity;
import android.content.Intent;
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
    private GameMenu gameMenu;
    private TextPrompt textPrompt;
    private boolean controlsVisible;
    private float gameRefreshRate=60f;
    // The frame rate the game is held to (RuntimeOptions.FPS_TARGET): 30 its own, 0 unlimited.
    private int fpsTarget=RuntimeOptions.FPS_ORIGINAL;
    // One per local player: the gamepad that plays as player one (together with the touch screen)
    // and the one that plays as player two. Null while no gamepad is assigned to that player.
    private final ExternalController.Player[] controllers = new ExternalController.Player[ExternalController.PLAYERS];
    // Connected gamepads that play as nobody (a third pad, or one the player switched off); their
    // events are dropped without looking at the assignment again.
    private final java.util.HashSet<Integer> spareIds = new java.util.HashSet<>();
    // The game's vibration, played on each player's gamepad (or the phone). Off when the player says so.
    private Rumble rumble;
    private boolean rumbleWanted = true;
    private InputManager inputManager;
    private final InputManager.InputDeviceListener deviceListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { if (launch) setupControllers(); }
        @Override public void onInputDeviceChanged(int id) { if (launch) setupControllers(); }
        @Override public void onInputDeviceRemoved(int id) { if (launch) setupControllers(); }
    };
    private ScrollView results;
    private boolean started, graphics, launch;
    private volatile boolean loaded, cancelled;
    private native String nativeValidate(String filesDirectory,Surface surface,String hooks,String directory,String library,boolean launch,boolean oneUiFix,boolean sceneMsaa,String tuDebug,String ir3Debug);
    private static native void nativeStop();
    private static native void nativeBackground();
    private static native boolean nativeSetPaused(boolean paused);
    private static native boolean nativeForeground(Surface surface);
    private static native void nativeButton(int button,boolean down);
    private static native void nativeStick(int stick,float x,float y);
    private static native void nativeTrigger(int trigger,float value);
    private static native void nativeExternalButton(int player,int button,boolean down);
    private static native void nativeExternalStick(int player,int stick,float x,float y);
    private static native void nativeExternalTrigger(int player,int trigger,float value);
    private static native void nativeSetPlayerAttached(int player,boolean attached);
    private static native long nativeRumble();
    private static native void nativeSetFpsTarget(int fps);
    private static native long nativeFrameCount();
    private static native long nativePrecompileProgress();
    private static native void nativeTextInputResult(String text,boolean cancelled);
    private TextView fps, temp;
    private ThermalReader thermal;
    private HudLayout.Profile hudProfile;
    // The player's switches (settings, sidebar); a readout also has to be visible in the layout profile.
    private boolean fpsWanted, tempWanted;
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
    /**
     * The game asked for the Xbox keyboard (naming a blueprint, for instance). Called from the
     * kernel's dispatch thread, which then waits for nativeTextInputResult; whatever happens here,
     * exactly one answer must go back or that thread stays blocked.
     */
    public static void requestTextInput(String title,String description,String text,int maxLength,int flags){
        RuntimeActivity activity=current==null?null:current.get();
        if(activity==null||activity.textPrompt==null){nativeTextInputResult("",true);return;}
        activity.runOnUiThread(()->{
            if(activity.isFinishing()||activity.textPrompt==null){nativeTextInputResult("",true);return;}
            if(activity.overlay!=null)activity.overlay.cancelTouches();
            activity.textPrompt.show(title,description,text,maxLength);
        });
    }
    private boolean textPromptShowing(){return textPrompt!=null&&textPrompt.isShowing();}
    public int openContentFd(String uri, String mode) throws java.io.IOException {
        android.os.ParcelFileDescriptor descriptor=getContentResolver().openFileDescriptor(android.net.Uri.parse(uri),mode);
        if(descriptor==null)throw new java.io.IOException(Language.s(R.string.file_unavailable));
        try {return descriptor.detachFd();} finally {descriptor.close();}
    }
    @Override protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(Language.attach(base));
    }

    @Override public void finish() {
        super.finish();
        Transitions.fadeClose(this);
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Transitions.fadeOpen(this);
        launch=getIntent().getBooleanExtra("launch",false);
        if(launch){
            fpsTarget=RuntimeOptions.fpsTarget(this);
            gameRefreshRate=refreshRateFor(fpsTarget);
        }
        if(launch)setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        graphics=launch||getIntent().getBooleanExtra("graphics",false);
        if(launch){
            inputManager=(InputManager)getSystemService(INPUT_SERVICE);
            inputManager.registerInputDeviceListener(deviceListener,null);
            setupControllers();
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
            applyImmersive();
            // The soft keyboard of the text box must not resize (or move) the game surface.
            getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
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
            // Frames the game presented (D3DDevice_Swap) per second, sampled every 0.5 s. Always built:
            // the game menu switches it on and off.
            fps=new TextView(this);fps.setTextColor(Color.rgb(120,255,120));fps.setTextSize(14);
            fps.setTypeface(android.graphics.Typeface.MONOSPACE,android.graphics.Typeface.BOLD);
            fps.setShadowLayer(3,1,1,Color.BLACK);fps.setText("FPS --");
            fpsWanted=getIntent().getBooleanExtra("fps",false);
            fps.setVisibility(fpsWanted?View.VISIBLE:View.GONE);
            // Both readouts sit where the screen layout profile says (applyHudLayout), measured from the top left.
            root.addView(fps,new FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.START));
            temp=new TextView(this);temp.setTextColor(Color.rgb(255,196,110));temp.setTextSize(13);
            temp.setTypeface(android.graphics.Typeface.MONOSPACE,android.graphics.Typeface.BOLD);
            temp.setShadowLayer(3,1,1,Color.BLACK);temp.setText("");
            tempWanted=getIntent().getBooleanExtra("temp",false);
            temp.setVisibility(tempWanted?View.VISIBLE:View.GONE);
            root.addView(temp,new FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.START));
            thermal=new ThermalReader(this);
            View.OnLayoutChangeListener replace=(v,l,t,r,bm,ol,ot,or,ob)->placeReadouts();
            root.addOnLayoutChangeListener(replace);fps.addOnLayoutChangeListener(replace);temp.addOnLayoutChangeListener(replace);
            applyHudLayout();
            // Temperatures change slowly: read the sensors off the UI thread every two seconds.
            final java.util.concurrent.ExecutorService sensors=java.util.concurrent.Executors.newSingleThreadExecutor(
                r->{Thread t=new Thread(r,"renut-thermal");t.setDaemon(true);return t;});
            temp.postDelayed(new Runnable(){@Override public void run(){
                if(isFinishing()){sensors.shutdown();return;}
                if(temp.getVisibility()==View.VISIBLE)sensors.execute(()->{
                    final String text=thermal.read();
                    runOnUiThread(()->{if(!text.isEmpty())temp.setText(text);});
                });
                temp.postDelayed(this,2000);
            }},500);
            final long[] last={-1,0};
            fps.postDelayed(new Runnable(){@Override public void run(){
                if(isFinishing())return;
                if(loaded&&fps.getVisibility()==View.VISIBLE){
                    long frames=nativeFrameCount(),now=android.os.SystemClock.elapsedRealtime();
                    if(last[0]>=0&&now>last[1])fps.setText(String.format(java.util.Locale.ROOT,"FPS %.1f",(frames-last[0])*1000.0/(now-last[1])));
                    last[0]=frames;last[1]=now;
                }else last[0]=-1;  // hidden or not loaded: the first reading after it is not an average
                fps.postDelayed(this,500);
            }},500);
            results.setVisibility(View.GONE);results.setBackgroundColor(Color.rgb(12,23,28));
            root.addView(results,new FrameLayout.LayoutParams(-1,-1));
            gameMenu=ComposeScreens.createGameMenu(this);
            root.addView(gameMenu.getView(),new FrameLayout.LayoutParams(-1,-1));
            rumble=new Rumble(this);
            rumbleWanted=ExternalController.rumbleEnabled(this);
            fps.postDelayed(new Runnable(){@Override public void run(){
                if(isFinishing())return;
                if(loaded){
                    // A frozen game keeps asking for its last rumble: the motors must not keep running.
                    boolean quiet=!rumbleWanted||inBackground||(gameMenu!=null&&gameMenu.isPaused());
                    rumble.apply(quiet?0:nativeRumble(),rumbleDevices());
                }
                fps.postDelayed(this,33);
            }},33);
            achievementToast=new AchievementToast(this);
            root.addView(achievementToast,AchievementToast.params(this));
            addPrecompileNote(root);
            textPrompt=new TextPrompt(this,(text,cancelled)->{
                applyImmersive();  // the keyboard may have brought the system bars back
                nativeTextInputResult(text,cancelled);
            });
            root.addView(textPrompt,new FrameLayout.LayoutParams(-1,-1));
            current=new java.lang.ref.WeakReference<>(this);
            setContentView(root);
            return;
        }
        LinearLayout body=new LinearLayout(this);body.setOrientation(LinearLayout.VERTICAL);
        int pad=(int)(20*getResources().getDisplayMetrics().density);
        body.setPadding(pad,pad*2,pad,pad);body.setBackgroundColor(Color.rgb(12,23,28));
        body.setOnApplyWindowInsetsListener((v,i)->{v.setPadding(pad,pad+i.getSystemWindowInsetTop(),pad,pad+i.getSystemWindowInsetBottom());return i;});
        status.setText(graphics?Language.s(R.string.runtime_graphics_status):Language.s(R.string.runtime_validating));
        body.addView(results,new LinearLayout.LayoutParams(-1,graphics?-2:0,graphics?0:1));
        if(graphics){surface=new SurfaceView(this);surface.getHolder().addCallback(this);body.addView(surface,new LinearLayout.LayoutParams(-1,0,1));}
        Button close=new Button(this);close.setText(Language.s(R.string.common_back));close.setOnClickListener(v->finish());body.addView(close);
        ComposeScreens.showRuntime(this);
        if(!graphics)startValidation(null);
    }
    /**
     * While a scene load waits for the pipelines built at start (plume-recipes.bin), the game stands
     * still; this card says why and how far along they are. It takes no touches and polls until the
     * building has finished.
     */
    private void addPrecompileNote(FrameLayout root){
        float density=getResources().getDisplayMetrics().density;
        LinearLayout card=new LinearLayout(this);card.setOrientation(LinearLayout.VERTICAL);
        int pad=Math.round(18*density);card.setPadding(pad,pad,pad,pad);
        android.graphics.drawable.GradientDrawable background=new android.graphics.drawable.GradientDrawable();
        background.setColor(Color.argb(230,28,30,33));background.setCornerRadius(18*density);
        card.setBackground(background);card.setClickable(false);card.setVisibility(View.GONE);
        TextView heading=new TextView(this);heading.setText(Language.s(R.string.precompile_title));
        heading.setTextColor(Color.rgb(244,240,235));heading.setTextSize(16);heading.setTypeface(null,android.graphics.Typeface.BOLD);
        card.addView(heading);
        TextView detail=new TextView(this);detail.setTextColor(Color.rgb(168,162,156));detail.setTextSize(13);
        card.addView(detail);
        ProgressBar bar=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        LinearLayout.LayoutParams barAt=new LinearLayout.LayoutParams(Math.round(260*density),-2);
        barAt.topMargin=Math.round(10*density);card.addView(bar,barAt);
        root.addView(card,new FrameLayout.LayoutParams(-2,-2,Gravity.CENTER));
        card.postDelayed(new Runnable(){@Override public void run(){
            if(isFinishing())return;
            if(loaded){
                long p=nativePrecompileProgress();
                int done=(int)(p&0xFFFFFF),total=(int)((p>>24)&0xFFFFFF);
                boolean running=(p>>48&1)!=0,waiting=(p>>49&1)!=0;
                if(waiting&&total>0){
                    detail.setText(Language.s(R.string.precompile_detail,done,total,done*100/total));
                    bar.setProgress((int)(done*1000L/total));
                    card.setVisibility(View.VISIBLE);
                }else card.setVisibility(View.GONE);
                if(total>0&&!running){card.setVisibility(View.GONE);return;}
            }
            card.postDelayed(this,250);
        }},250);
    }
    private void applyImmersive(){
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
            |View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            |View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
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
    /** Reads the selected screen layout profile and puts the controller and the readouts where it says. */
    private void applyHudLayout(){
        if(!launch||overlay==null)return;
        hudProfile=HudLayout.load(this);
        overlay.setLayout(hudProfile);  // each control carries its own opacity
        fps.setVisibility(fpsWanted&&HudLayout.visible(hudProfile,HudLayout.FPS)?View.VISIBLE:View.GONE);
        temp.setVisibility(tempWanted&&HudLayout.visible(hudProfile,HudLayout.TEMP)?View.VISIBLE:View.GONE);
        placeReadouts();
    }
    private void placeReadouts(){
        if(fps==null||temp==null||hudProfile==null)return;
        View root=(View)fps.getParent();
        int w=root.getWidth(),h=root.getHeight();
        if(w<=0||h<=0)return;
        placeReadout(fps,HudLayout.FPS,w,h);
        placeReadout(temp,HudLayout.TEMP,w,h);
    }
    private void placeReadout(TextView view,String id,int w,int h){
        HudLayout.Shape shape=HudLayout.shape(hudProfile,id,w,h);
        HudLayout.Item item=hudProfile.items.get(id);
        float scale=item==null?1f:item.scale;
        view.setScaleX(scale);view.setScaleY(scale);
        view.setAlpha(HudLayout.opacity(hudProfile,id));
        view.setTranslationX(shape.cx-view.getWidth()/2f);
        view.setTranslationY(shape.cy-view.getHeight()/2f);
    }
    public boolean uiTempVisible(){return tempWanted;}
    public void uiSetTempVisible(boolean visible){
        if(temp==null)return;
        tempWanted=visible;
        temp.setVisibility(visible&&HudLayout.visible(hudProfile,HudLayout.TEMP)?View.VISIBLE:View.GONE);
        getSharedPreferences("MainActivity",MODE_PRIVATE).edit().putBoolean("temp",visible).apply();
    }
    /** The sources the readout can use on this device, for the sidebar (e.g. "CPU, GPU, BAT"). */
    public String uiTemperatureSources(){return thermal==null?"":thermal.sources();}
    /** Opens the screen layout editor over the game; the game stays alive behind it, as when the app is left. */
    public void uiOpenLayoutEditor(){
        if(!launch)return;
        startActivity(new Intent(this,LayoutEditorActivity.class));
    }
    /**
     * The display mode the screen should run at for a frame rate. Up to 60 the default rate keeps an
     * even cadence (30 is every second refresh); above it (or unlimited) the panel's fastest mode
     * of the current resolution, so that its refresh rate is not what holds the game back.
     */
    private float refreshRateFor(int fps){
        float rate=60f;
        if(fps!=0&&fps<=60)return rate;
        Display display=getDisplay();
        if(display==null)return rate;
        Display.Mode current=display.getMode();
        for(Display.Mode mode:display.getSupportedModes()){
            if(mode.getPhysicalWidth()==current.getPhysicalWidth() &&
               mode.getPhysicalHeight()==current.getPhysicalHeight())
                rate=Math.max(rate,mode.getRefreshRate());
        }
        return rate;
    }
    public int uiFpsTarget(){return fpsTarget;}
    /**
     * Live while the slider moves: the game takes the new rate at its next frame, and the screen
     * is asked for the refresh rate that goes with it. Nothing is saved (uiSaveFpsTarget does that
     * when the finger lifts).
     */
    public void uiSetFpsTarget(int fps){
        fpsTarget=fps;
        if(loaded)nativeSetFpsTarget(fps);
        float rate=refreshRateFor(fps);
        if(rate==gameRefreshRate)return;
        gameRefreshRate=rate;
        if(!launch)return;
        WindowManager.LayoutParams params=getWindow().getAttributes();
        params.preferredRefreshRate=rate;
        getWindow().setAttributes(params);
        if(android.os.Build.VERSION.SDK_INT>=30&&surface!=null&&surface.getHolder().getSurface().isValid()){
            try{surface.getHolder().getSurface().setFrameRate(rate,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);}catch(Exception ignored){}
        }
    }
    public void uiSaveFpsTarget(int fps){
        try{RuntimeOptions.set(this,RuntimeOptions.FPS_TARGET,Integer.toString(fps));}
        catch(java.io.IOException e){Toast.makeText(this,Language.s(R.string.toml_save_failed,e.getMessage()),Toast.LENGTH_SHORT).show();}
    }
    public boolean uiFpsVisible(){return fpsWanted;}
    public void uiSetFpsVisible(boolean visible){
        if(fps==null)return;
        fpsWanted=visible;
        fps.setVisibility(visible&&HudLayout.visible(hudProfile,HudLayout.FPS)?View.VISIBLE:View.GONE);
        getSharedPreferences("MainActivity",MODE_PRIVATE).edit().putBoolean("fps",visible).apply();
    }
    /** The player's pause. False when the game could not take it (not drawing yet), so the menu shows the truth. */
    public boolean uiSetGamePaused(boolean paused){
        if(!launch||!loaded)return false;
        return nativeSetPaused(paused);
    }
    private void showGameMenu(){
        if(!launch||gameMenu==null||results.getVisibility()==View.VISIBLE)return;
        overlay.cancelTouches();
        gameMenu.open();
    }
    @Override public void onBackPressed(){
        if(textPromptShowing()){textPrompt.cancel();return;}
        if(launch&&gameMenu!=null&&gameMenu.back())return;
        if(launch&&results.getVisibility()!=View.VISIBLE){showGameMenu();return;}
        super.onBackPressed();
    }
    /**
     * Brings the players' gamepads in line with what is connected and with the player's choices
     * (assign): a pad that is no longer the one for its player lets go of its buttons, a new one
     * starts. Cheap when nothing changed, so it may be called on any device event.
     */
    private void setupControllers(){
        InputDevice[] assigned=new InputDevice[ExternalController.PLAYERS];
        rumbleWanted=ExternalController.rumbleEnabled(this);
        boolean changed=false;
        spareIds.clear();
        for(ExternalController.Pad pad:ExternalController.pads(this)){
            if(pad.role>=0)assigned[pad.role]=pad.device;else spareIds.add(pad.device.getId());
        }
        for(int player=0;player<ExternalController.PLAYERS;player++){
            final int slot=player;
            ExternalController.Player current=controllers[slot];
            InputDevice device=assigned[slot];
            if(current!=null&&device!=null&&current.deviceId()==device.getId())continue;
            changed=true;
            if(current!=null){current.release();controllers[slot]=null;}
            if(device==null)continue;
            controllers[slot]=new ExternalController.Player(device,ExternalController.load(this,device),new ExternalController.Sink(){
                @Override public void button(int mask,boolean down){if(loaded)nativeExternalButton(slot,mask,down);}
                @Override public void stick(int stick,float x,float y){if(loaded)nativeExternalStick(slot,stick,x,y);}
                @Override public void trigger(int trigger,float value){if(loaded)nativeExternalTrigger(slot,trigger,value);}
            });
        }
        if(changed&&rumble!=null)rumble.stop();  // the motors that were running belong to another pad now
        syncSecondPlayer();
    }
    /** The gamepad each player is playing on, null for none (the touch screen alone). */
    private InputDevice[] rumbleDevices(){
        InputDevice[] devices=new InputDevice[ExternalController.PLAYERS];
        for(int player=0;player<ExternalController.PLAYERS;player++)
            if(controllers[player]!=null)devices[player]=controllers[player].device();
        return devices;
    }
    public boolean uiRumbleEnabled(){return rumbleWanted;}
    public void uiSetRumble(boolean enabled){
        rumbleWanted=enabled;
        ExternalController.setRumbleEnabled(this,enabled);
        if(!enabled&&rumble!=null)rumble.stop();
    }
    /** Tells the game whether a second player's pad is there; it learns of the change on its next poll. */
    private void syncSecondPlayer(){
        if(loaded)nativeSetPlayerAttached(ExternalController.PLAYER_TWO,controllers[ExternalController.PLAYER_TWO]!=null);
    }
    private void clearControllers(){
        if(rumble!=null)rumble.stop();
        for(int player=0;player<ExternalController.PLAYERS;player++){
            if(controllers[player]!=null)controllers[player].release();
            controllers[player]=null;
        }
    }
    /** The menus changed who plays as whom. */
    public void uiControllersChanged(){if(launch)setupControllers();}
    private boolean routeKey(KeyEvent event){
        for(ExternalController.Player player:controllers)if(player!=null&&player.key(event))return true;
        return false;
    }
    private boolean routeMotion(MotionEvent event){
        for(ExternalController.Player player:controllers)if(player!=null&&player.motion(event))return true;
        return false;
    }
    /** A gamepad no player has yet: connected without the listener hearing of it, or just assigned. */
    private boolean unknownPad(InputDevice device){
        if(!ExternalController.isGamepad(device)||spareIds.contains(device.getId()))return false;
        for(ExternalController.Player player:controllers)if(player!=null&&player.accepts(device))return false;
        return true;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        if(launch&&event.getKeyCode()==KeyEvent.KEYCODE_BACK){
            if(event.getAction()==KeyEvent.ACTION_UP)onBackPressed();
            return true;
        }
        if(launch&&(textPromptShowing()||gameMenu!=null&&gameMenu.isOpen()))return super.dispatchKeyEvent(event);
        if(launch){
            if(unknownPad(event.getDevice()))setupControllers();
            if(routeKey(event))return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event){
        if(launch&&!textPromptShowing()){
            if(unknownPad(event.getDevice()))setupControllers();
            if(routeMotion(event))return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }
    private void startValidation(Surface target){
        if(started||cancelled)return;started=true;
        new Thread(()->{
            String report;
            try {
                System.loadLibrary("SDL3");org.libsdl.app.SDL.setupJNI();org.libsdl.app.SDL.initialize();org.libsdl.app.SDL.setContext(this);
                System.loadLibrary("renut_runtime");loaded=true;
                runOnUiThread(this::syncSecondPlayer);  // a second pad that was already plugged in
                if(cancelled){nativeStop();return;}
                report=nativeValidate(getFilesDir().getAbsolutePath(),target,getApplicationInfo().nativeLibraryDir,
                    getIntent().getStringExtra("driverDir"),getIntent().getStringExtra("driverLib"),
                    launch,getIntent().getBooleanExtra("oneUiFix",true),
                    getIntent().getBooleanExtra("sceneMsaa",false),
                    getIntent().getStringExtra("tuDebug"),getIntent().getStringExtra("ir3Debug"));
            }catch(Throwable e){
                report=Language.s(R.string.runtime_start_failed,String.valueOf(e));
                try{Files.write(new File(getFilesDir(),"runtime-report.txt").toPath(),report.getBytes(StandardCharsets.UTF_8));}catch(Exception ignored){}
            }
            final String message=report;
            runOnUiThread(()->{if(!isFinishing()){status.setText(message);if(surface!=null)surface.setVisibility(View.GONE);
                if(launch){if(overlay!=null)overlay.setVisibility(View.GONE);if(fps!=null)fps.setVisibility(View.GONE);if(temp!=null)temp.setVisibility(View.GONE);
                    if(gameMenu!=null)gameMenu.getView().setVisibility(View.GONE);results.setVisibility(View.VISIBLE);}
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
            if(nativeForeground(h.getSurface())){
                inBackground=false;
                if(gameMenu!=null&&gameMenu.isPaused())nativeSetPaused(true);  // coming back must not undo the pause
            }
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
    @Override protected void onPause(){clearControllers();super.onPause();if(!launch){stop();finish();}}
    @Override protected void onResume(){super.onResume();if(launch){setupControllers();applyHudLayout();}}
    @Override protected void onDestroy(){if(rumble!=null)rumble.stop();if(inputManager!=null)inputManager.unregisterInputDeviceListener(deviceListener);stop();if(launch)KeepAliveService.stop(this);super.onDestroy();if(isFinishing())android.os.Process.killProcess(android.os.Process.myPid());}
}
