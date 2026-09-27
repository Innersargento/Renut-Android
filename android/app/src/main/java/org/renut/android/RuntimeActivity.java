package org.renut.android;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.SurfaceTexture;
import android.hardware.input.InputManager;
import android.view.*;
import android.widget.*;
import java.io.File;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;

/** Isolates the Vulkan loader and the guest runtime for each validation session. */
public final class RuntimeActivity extends Activity implements SurfaceHolder.Callback {
    private TextView status;
    private SurfaceView surface;
    private GamepadOverlay overlay;
    private TextureView movieView;
    // Movie overlay state, touched only on the UI thread. Times are System.nanoTime().
    private Thread movieWorker, lastMovieWorker;
    private File movieFile;
    private String missingMoviePath;
    private long lastMovieFrame, movieStart;
    private int movieSerial=-1, finishedMovieSerial=-1;
    private int movieGeneration;
    private final android.os.Handler movieHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable movieWatchdog = new Runnable(){@Override public void run(){
        if(movieSerial<0)return;
        // The game stopped showing it (skipped or over): do not restart it midway.
        if(System.nanoTime()-lastMovieFrame>1_800_000_000L){finishedMovieSerial=movieSerial;stopMovie();return;}
        movieHandler.postDelayed(this,500);
    }};
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
    private native String nativeValidate(String filesDirectory,Surface surface,String hooks,String directory,String library,String renderer,boolean launch,boolean oneUiFix,boolean sceneMsaa,String tuDebug,String ir3Debug);
    private static native void nativeStop();
    private static native void nativeButton(int button,boolean down);
    private static native void nativeStick(int stick,float x,float y);
    private static native void nativeTrigger(int trigger,float value);
    private static native void nativeExternalButton(int button,boolean down);
    private static native void nativeExternalStick(int stick,float x,float y);
    private static native void nativeExternalTrigger(int trigger,float value);
    private static native long nativeFrameCount();
    private TextView fps;
    public int openContentFd(String uri, String mode) throws java.io.IOException {
        android.os.ParcelFileDescriptor descriptor=getContentResolver().openFileDescriptor(android.net.Uri.parse(uri),mode);
        if(descriptor==null)throw new java.io.IOException("Arquivo indisponível.");
        try {return descriptor.detachFd();} finally {descriptor.close();}
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        launch=getIntent().getBooleanExtra("launch",false);
        if(launch)setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        graphics=launch||getIntent().getBooleanExtra("graphics",false);
        if(launch){
            inputManager=(InputManager)getSystemService(INPUT_SERVICE);
            inputManager.registerInputDeviceListener(deviceListener,null);
            setupController();
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        status=new TextView(this);status.setTextColor(Color.rgb(226,237,235));status.setTextSize(16);
        results=new ScrollView(this);results.addView(status);
        if(launch){
            // Full screen game surface with optional on-screen controls.
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                |View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                |View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            if(android.os.Build.VERSION.SDK_INT>=28)getWindow().getAttributes().layoutInDisplayCutoutMode=
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().getAttributes().preferredRefreshRate=60f;
            FrameLayout root=new FrameLayout(this);root.setBackgroundColor(Color.BLACK);
            surface=new SurfaceView(this);surface.getHolder().addCallback(this);
            root.addView(surface,new FrameLayout.LayoutParams(-1,-1));
            movieView=new TextureView(this);
            movieView.setOpaque(true);
            movieView.setVisibility(View.GONE);
            movieView.setSurfaceTextureListener(new TextureView.SurfaceTextureListener(){
                @Override public void onSurfaceTextureAvailable(SurfaceTexture texture,int width,int height){openMovie();}
                @Override public void onSurfaceTextureSizeChanged(SurfaceTexture texture,int width,int height){}
                @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture texture){if(movieWorker!=null)movieWorker.interrupt();return true;}
                @Override public void onSurfaceTextureUpdated(SurfaceTexture texture){}
            });
            root.addView(movieView,new FrameLayout.LayoutParams(-1,-1));
            if(getIntent().getBooleanExtra("showControls",true)){
                overlay=new GamepadOverlay(this,new GamepadOverlay.Sink(){
                    @Override public void button(int mask,boolean down){if(loaded)nativeButton(mask,down);}
                    @Override public void stick(int stick,float x,float y){if(loaded)nativeStick(stick,x,y);}
                    @Override public void trigger(int trigger,float value){if(loaded)nativeTrigger(trigger,value);}
                    @Override public void exit(){finish();}
                });
                root.addView(overlay,new FrameLayout.LayoutParams(-1,-1));
            }
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
        setContentView(body);
        if(!graphics)startValidation(null);
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
    /** Called on the guest thread by the movie blit hook, at most every 500 ms; serial names one playback. */
    public void onMovieFrame(String gamePath,int serial){
        if(!launch||gamePath==null)return;
        final long now=System.nanoTime();
        runOnUiThread(()->{
            lastMovieFrame=now;
            if(serial==movieSerial||serial==finishedMovieSerial)return;
            File source=gamePath.equals(missingMoviePath)?null:sourceMovie(gamePath);
            if(source==null){
                if(!gamePath.equals(missingMoviePath))android.util.Log.w("renut-video","Vídeo não encontrado nos dados do jogo: "+gamePath);
                missingMoviePath=gamePath;
                finishedMovieSerial=serial;
                return;
            }
            stopMovie();
            movieSerial=serial;
            movieFile=source;
            movieStart=now;
            movieView.setVisibility(View.VISIBLE);
            if(movieView.isAvailable())openMovie();
            movieHandler.postDelayed(movieWatchdog,500);
        });
    }
    /** Maps a guest path such as GAME:\\dir\\file onto the imported game data, ignoring case like the guest filesystem. */
    private File sourceMovie(String guestPath){
        String relative=guestPath.replace('\\','/');
        int device=relative.indexOf(':');
        if(device>=0)relative=relative.substring(device+1);
        try{
            File files=getFilesDir().getCanonicalFile();
            String active=new String(Files.readAllBytes(new File(files,"active-game.txt").toPath()),StandardCharsets.UTF_8).trim();
            File data=new File(files,active).getCanonicalFile();
            if(!data.getPath().startsWith(files.getPath()+File.separator))return null;
            File source=data;
            for(String part:relative.split("/")){
                if(part.isEmpty()||part.equals("."))continue;
                if(part.equals(".."))return null;
                File next=new File(source,part);
                if(!next.exists()){
                    String[] names=source.list();
                    if(names==null)return null;
                    next=null;
                    for(String name:names)if(name.equalsIgnoreCase(part)){next=new File(source,name);break;}
                    if(next==null)return null;
                }
                source=next;
            }
            source=source.getCanonicalFile();
            return source.isFile()&&source.getPath().startsWith(data.getPath()+File.separator)?source:null;
        }catch(Exception ignored){return null;}
    }
    private void openMovie(){
        if(movieFile==null||movieView==null||!movieView.isAvailable()||movieWorker!=null)return;
        final int generation=++movieGeneration;
        final String source=movieFile.getAbsolutePath();
        final long start=movieStart;
        final SurfaceTexture texture=movieView.getSurfaceTexture();
        // A stopped worker may still be connected to this SurfaceTexture, and
        // a second producer cannot connect until it lets go.
        final Thread previous=lastMovieWorker;
        Thread worker=new Thread(()->{
            String failure=null;
            try{
                if(previous!=null)previous.join();
                if(Thread.currentThread().isInterrupted())return;
                Surface videoSurface=new Surface(texture);
                try{failure=NativeMoviePlayer.play(source,videoSurface,Thread.currentThread(),this,start);}
                finally{videoSurface.release();}
            }catch(InterruptedException ignored){
            }catch(RuntimeException exception){failure=String.valueOf(exception);
            }finally{
                if(failure!=null)android.util.Log.e("renut-video",source+": "+failure);
                runOnUiThread(()->{if(generation==movieGeneration&&movieSerial>=0){finishedMovieSerial=movieSerial;stopMovie();}});
            }
        },"renut-native-movie");
        movieWorker=worker;
        lastMovieWorker=worker;
        worker.start();
    }
    public void onNativeMovieSize(int width,int height){
        runOnUiThread(()->{
            if(movieView==null||movieSerial<0)return;
            float viewRatio=(float)movieView.getWidth()/Math.max(1,movieView.getHeight());
            float videoRatio=(float)width/Math.max(1,height);
            Matrix transform=new Matrix();
            if(viewRatio>videoRatio)transform.setScale(videoRatio/viewRatio,1f,movieView.getWidth()/2f,movieView.getHeight()/2f);
            else transform.setScale(1f,viewRatio/videoRatio,movieView.getWidth()/2f,movieView.getHeight()/2f);
            movieView.setTransform(transform);
        });
    }
    private void stopMovie(){
        movieHandler.removeCallbacks(movieWatchdog);
        if(movieWorker!=null){movieWorker.interrupt();movieWorker=null;}
        movieGeneration++;
        movieFile=null;movieSerial=-1;
        if(movieView!=null)movieView.setVisibility(View.GONE);
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){
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
                    getIntent().getStringExtra("renderer"),launch,getIntent().getBooleanExtra("oneUiFix",true),
                    getIntent().getBooleanExtra("sceneMsaa",false),
                    getIntent().getStringExtra("tuDebug"),getIntent().getStringExtra("ir3Debug"));
            }catch(Throwable e){
                report="Falha ao iniciar o runtime: "+e;
                try{Files.write(new File(getFilesDir(),"runtime-report.txt").toPath(),report.getBytes(StandardCharsets.UTF_8));}catch(Exception ignored){}
            }
            final String message=report;
            runOnUiThread(()->{if(!isFinishing()){stopMovie();status.setText(message);if(surface!=null)surface.setVisibility(View.GONE);
                if(launch){if(overlay!=null)overlay.setVisibility(View.GONE);if(fps!=null)fps.setVisibility(View.GONE);results.setVisibility(View.VISIBLE);}
                else results.setLayoutParams(new LinearLayout.LayoutParams(-1,0,1));}});
        },"renut-runtime-validation").start();
    }
    private void stop(){cancelled=true;if(loaded)nativeStop();}
    @Override public void surfaceCreated(SurfaceHolder h){
        // The game presents at most 30 frames a second; left alone the panel
        // refreshes at 120 Hz for them. 60 keeps an even cadence below 30 too.
        if(launch&&android.os.Build.VERSION.SDK_INT>=30){
            try{h.getSurface().setFrameRate(60f,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);}catch(Exception ignored){}
        }
        startValidation(h.getSurface());
    }
    @Override public void surfaceChanged(SurfaceHolder h,int format,int width,int height){}
    @Override public void surfaceDestroyed(SurfaceHolder h){stop();}
    @Override protected void onPause(){stopMovie();clearController();stop();super.onPause();finish();}
    @Override protected void onDestroy(){if(inputManager!=null)inputManager.unregisterInputDeviceListener(deviceListener);stop();super.onDestroy();if(isFinishing())android.os.Process.killProcess(android.os.Process.myPid());}
}
