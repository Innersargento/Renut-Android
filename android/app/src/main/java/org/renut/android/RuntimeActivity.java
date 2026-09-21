package org.renut.android;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.Color;
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
    private ScrollView results;
    private boolean started, graphics, launch;
    private volatile boolean loaded, cancelled;
    private native String nativeValidate(String filesDirectory,Surface surface,String hooks,String directory,String library,String renderer,boolean launch,boolean oneUiFix,String tuDebug);
    private static native void nativeStop();
    private static native void nativeButton(int button,boolean down);
    private static native void nativeStick(int stick,float x,float y);
    private static native void nativeTrigger(int trigger,float value);
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
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        status=new TextView(this);status.setTextColor(Color.rgb(226,237,235));status.setTextSize(16);
        results=new ScrollView(this);results.addView(status);
        if(launch){
            // Full screen: the game surface with the Xbox 360 controller drawn over it.
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                |View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                |View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            if(android.os.Build.VERSION.SDK_INT>=28)getWindow().getAttributes().layoutInDisplayCutoutMode=
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            FrameLayout root=new FrameLayout(this);root.setBackgroundColor(Color.BLACK);
            surface=new SurfaceView(this);surface.getHolder().addCallback(this);
            root.addView(surface,new FrameLayout.LayoutParams(-1,-1));
            overlay=new GamepadOverlay(this,new GamepadOverlay.Sink(){
                @Override public void button(int mask,boolean down){if(loaded)nativeButton(mask,down);}
                @Override public void stick(int stick,float x,float y){if(loaded)nativeStick(stick,x,y);}
                @Override public void trigger(int trigger,float value){if(loaded)nativeTrigger(trigger,value);}
                @Override public void exit(){finish();}
            });
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
                    getIntent().getStringExtra("tuDebug"));
            }catch(Throwable e){
                report="Falha ao iniciar o runtime: "+e;
                try{Files.write(new File(getFilesDir(),"runtime-report.txt").toPath(),report.getBytes(StandardCharsets.UTF_8));}catch(Exception ignored){}
            }
            final String message=report;
            runOnUiThread(()->{if(!isFinishing()){status.setText(message);if(surface!=null)surface.setVisibility(View.GONE);
                if(overlay!=null){overlay.setVisibility(View.GONE);if(fps!=null)fps.setVisibility(View.GONE);results.setVisibility(View.VISIBLE);}
                else results.setLayoutParams(new LinearLayout.LayoutParams(-1,0,1));}});
        },"renut-runtime-validation").start();
    }
    private void stop(){cancelled=true;if(loaded)nativeStop();}
    @Override public void surfaceCreated(SurfaceHolder h){startValidation(h.getSurface());}
    @Override public void surfaceChanged(SurfaceHolder h,int format,int width,int height){}
    @Override public void surfaceDestroyed(SurfaceHolder h){stop();}
    @Override protected void onPause(){stop();super.onPause();finish();}
    @Override protected void onDestroy(){stop();super.onDestroy();if(isFinishing())android.os.Process.killProcess(android.os.Process.myPid());}
}
