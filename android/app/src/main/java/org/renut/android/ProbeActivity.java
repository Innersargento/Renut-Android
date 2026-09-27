package org.renut.android;

import android.app.Activity;
import android.os.Bundle;
import android.view.*;
import android.widget.*;
import java.io.File;

/** A fresh process selects one Vulkan loader; exiting discards all driver state. */
public final class ProbeActivity extends androidx.activity.ComponentActivity implements SurfaceHolder.Callback {
    static { System.loadLibrary("renut_probe"); }
    private static native String nativeRun(Surface surface,String hooks,String directory,String library);
    private static native void nativeStop();
    private Thread render;
    private boolean started;
    private TextView status;
    private SurfaceView surface;
    private ScrollView results;
    @Override public void onCreate(Bundle state){
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout box=new LinearLayout(this);box.setOrientation(LinearLayout.VERTICAL);box.setPadding(24,48,24,32);
        box.setOnApplyWindowInsetsListener((v,i)->{v.setPadding(24,24+i.getSystemWindowInsetTop(),24,24+i.getSystemWindowInsetBottom());return i;});
        status=new TextView(this);status.setText("Testando apresentação Plume…\nA animação dura alguns segundos.");
        results=new ScrollView(this);results.addView(status);box.addView(results);
        surface=new SurfaceView(this);surface.getHolder().addCallback(this);box.addView(surface,new LinearLayout.LayoutParams(-1,0,1));
        Button close=new Button(this);close.setText("Voltar à biblioteca");close.setOnClickListener(v->finish());box.addView(close);
        ComposeScreens.showProbe(this);
    }
    public String uiStatus(){return status.getText().toString();}
    public boolean uiFinished(){return !started||render!=null&&!render.isAlive()&&status.getText().toString().length()>0;}
    public void attachComposeSurface(SurfaceView view){surface=view;surface.getHolder().addCallback(this);}
    @Override public void surfaceCreated(SurfaceHolder holder){
        if(started)return;started=true;
        render=new Thread(()->{
            String result=nativeRun(holder.getSurface(),getApplicationInfo().nativeLibraryDir,
                getIntent().getStringExtra("driverDir"),getIntent().getStringExtra("driverLib"));
            try{GameFiles.atomicWrite(new File(getFilesDir(),"probe-report.txt"),result);}catch(Exception e){result+="\nNão foi possível salvar o diagnóstico.";}
            final String message=result;runOnUiThread(()->{status.setText(message);surface.setVisibility(View.GONE);results.setLayoutParams(new LinearLayout.LayoutParams(-1,0,1));});
        },"renut-vulkan");render.start();
    }
    @Override public void surfaceChanged(SurfaceHolder h,int format,int width,int height){}
    @Override public void surfaceDestroyed(SurfaceHolder holder){nativeStop();}
    @Override protected void onPause(){nativeStop();super.onPause();finish();}
    @Override protected void onDestroy(){
        nativeStop();super.onDestroy();
        // The driver is never unloaded or switched in a live process.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
