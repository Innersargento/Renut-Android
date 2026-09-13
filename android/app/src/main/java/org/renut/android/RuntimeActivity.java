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
    private ScrollView results;
    private boolean started, graphics, launch;
    private volatile boolean loaded, cancelled;
    private native String nativeValidate(String filesDirectory,Surface surface,String hooks,String directory,String library,boolean launch);
    private static native void nativeStop();
    private static native void nativeButton(int button,boolean down);
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
        LinearLayout body=new LinearLayout(this);body.setOrientation(LinearLayout.VERTICAL);
        int pad=(int)(20*getResources().getDisplayMetrics().density);
        body.setPadding(pad,pad*2,pad,pad);body.setBackgroundColor(Color.rgb(12,23,28));
        body.setOnApplyWindowInsetsListener((v,i)->{v.setPadding(pad,pad+i.getSystemWindowInsetTop(),pad,pad+i.getSystemWindowInsetBottom());return i;});
        status=new TextView(this);status.setTextColor(Color.rgb(226,237,235));status.setTextSize(16);
        status.setText(launch?"Banjo-Kazooie: Nuts & Bolts · Experimental":graphics?"RexGlue + Plume\nImagem de teste na memória do runtime. Ainda não representa cenas do jogo.":"Validando o runtime ARM64…\nEste teste ainda não inicia o jogo.");
        results=new ScrollView(this);results.addView(status);
        body.addView(results,new LinearLayout.LayoutParams(-1,graphics?-2:0,graphics?0:1));
        if(graphics){surface=new SurfaceView(this);surface.getHolder().addCallback(this);body.addView(surface,new LinearLayout.LayoutParams(-1,0,1));}
        if(launch){
            LinearLayout controls=new LinearLayout(this);
            String[] labels={"↑","↓","←","→","Start","A","B"};int[] masks={1,2,4,8,16,4096,8192};
            for(int index=0;index<labels.length;index++){
                Button key=new Button(this);key.setText(labels[index]);final int mask=masks[index];
                key.setOnTouchListener((v,event)->{
                    int action=event.getActionMasked();
                    if(action==MotionEvent.ACTION_DOWN){if(loaded)nativeButton(mask,true);return true;}
                    if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL){if(loaded)nativeButton(mask,false);if(action==MotionEvent.ACTION_UP)v.performClick();return true;}
                    return true;
                });
                controls.addView(key,new LinearLayout.LayoutParams(0,-2,1));
            }
            body.addView(controls);
        }
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
                    getIntent().getStringExtra("driverDir"),getIntent().getStringExtra("driverLib"),launch);
            }catch(Throwable e){
                report="Falha ao iniciar o runtime: "+e;
                try{Files.write(new File(getFilesDir(),"runtime-report.txt").toPath(),report.getBytes(StandardCharsets.UTF_8));}catch(Exception ignored){}
            }
            final String message=report;
            runOnUiThread(()->{if(!isFinishing()){status.setText(message);if(surface!=null)surface.setVisibility(View.GONE);results.setLayoutParams(new LinearLayout.LayoutParams(-1,0,1));}});
        },"renut-runtime-validation").start();
    }
    private void stop(){cancelled=true;if(loaded)nativeStop();}
    @Override public void surfaceCreated(SurfaceHolder h){startValidation(h.getSurface());}
    @Override public void surfaceChanged(SurfaceHolder h,int format,int width,int height){}
    @Override public void surfaceDestroyed(SurfaceHolder h){stop();}
    @Override protected void onPause(){stop();super.onPause();finish();}
    @Override protected void onDestroy(){stop();super.onDestroy();if(isFinishing())android.os.Process.killProcess(android.os.Process.myPid());}
}
