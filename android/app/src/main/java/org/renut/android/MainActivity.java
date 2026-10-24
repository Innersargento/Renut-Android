package org.renut.android;

import android.app.*;
import android.content.*;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.*;
import android.view.*;
import android.widget.*;
import org.json.JSONObject;
import java.io.*;
import java.nio.file.Files;
import java.util.*;
import java.util.zip.*;

public final class MainActivity extends androidx.activity.ComponentActivity {
    private static final int DRIVER=12, SAVE_IMPORT=14, SAVE_EXPORT=15, PHOTO_EXPORT=16;
    private volatile String status="";
    private volatile Thread worker;
    private long lastProgress;

    @Override protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(Language.attach(base));
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Transitions.fadeOpen(this);  // arriving from the first-run screens or leaving the game
        // No game yet: the first-run screens introduce the project and install it. They hand
        // over to this launcher (a new instance) once a game is imported.
        if(!hasGame()){
            startActivity(new Intent(this,WelcomeActivity.class));
            finish();
            return;
        }
        BundledDriver.sync(this,getPreferences(0));  // unpacks the Turnip that ships in the APK and makes it the default on Adreno
        getWindow().setStatusBarColor(Color.rgb(16,10,6));
        getWindow().setNavigationBarColor(Color.rgb(16,10,6));
        refresh();
        ComposeScreens.showMain(this);
        // Only on a fresh launch of the launcher: leaving the game (back, or "Sair para o
        // menu principal") resumes this activity instead of creating it, so the launcher
        // stays reachable to turn the option off.
        if(state==null&&!getIntent().getBooleanExtra("noAutoStart",false)&&uiAutoStart()&&hasGame())startGame();
    }
    private boolean hasGame(){
        try{
            String name=new String(Files.readAllBytes(new File(getFilesDir(),"active-game.txt").toPath()),java.nio.charset.StandardCharsets.UTF_8).trim();
            return !name.isEmpty()&&new File(getFilesDir(),name).isDirectory();
        }catch(IOException e){return false;}
    }
    /** 0: nothing imported, 1: only the XEX, 2: the full disc, so the game can start. */
    public int uiGameState(){
        if(!hasGame())return 0;
        try{
            String name=new String(Files.readAllBytes(new File(getFilesDir(),"active-game.txt").toPath()),java.nio.charset.StandardCharsets.UTF_8).trim();
            return new File(new File(getFilesDir(),name),"Bundle").isDirectory()?2:1;
        }catch(IOException e){return 0;}
    }
    public void uiStartGame(){if(worker==null)startGame();}
    /** Opens the import flow of the first-run screens (without their introduction). */
    public void uiOpenGameImport(){
        if(worker!=null)return;
        startActivity(new Intent(this,WelcomeActivity.class).putExtra(WelcomeActivity.EXTRA_IMPORT_ONLY,true));
    }
    private void startGame(){
        Intent i=new Intent(this,RuntimeActivity.class).putExtra("launch",true);
        i.putExtra("driverDir",getPreferences(0).getString("driverDir",""));
        i.putExtra("driverLib",getPreferences(0).getString("driverLib",""));
        i.putExtra("fps",getPreferences(0).getBoolean("fps",false));
        i.putExtra("temp",getPreferences(0).getBoolean("temp",false));
        i.putExtra("showControls",getPreferences(0).getBoolean("showControls",true));
        boolean turnip=uiDriverIsTurnip();
        i.putExtra("oneUiFix",turnip&&getPreferences(0).getBoolean("oneui",true));
        i.putExtra("sceneMsaa",false);
        i.putExtra("tuDebug",turnip?getPreferences(0).getString("tuDebug",""):"");
        i.putExtra("ir3Debug",turnip?getPreferences(0).getString("ir3Debug",""):"");
        startActivity(i);
    }
    private void toast(String message){
        runOnUiThread(()->Toast.makeText(this,message,Toast.LENGTH_LONG).show());
    }
    public void uiExportSaves(){
        if(worker!=null)return;
        Intent i=new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE);
        i.putExtra(Intent.EXTRA_TITLE,"renut-saves.zip");startActivityForResult(i,SAVE_EXPORT);
    }
    public void uiImportSaves(){
        if(worker!=null)return;
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE),SAVE_IMPORT);
    }
    public void uiPickDriver(){if(worker==null)pick(DRIVER);}
    public void uiOpenControllerSettings(){if(worker==null)startActivity(new Intent(this,ControllerSettingsActivity.class));}
    public void uiConfirmClearShaderCache(){
        if(worker!=null)return;
        new AlertDialog.Builder(this)
            .setTitle(Language.s(R.string.shader_cache_title))
            .setMessage(Language.s(R.string.shader_cache_message))
            .setNegativeButton(Language.s(R.string.common_cancel),null)
            .setPositiveButton(Language.s(R.string.common_delete),(dialog,which)->clearShaderCache())
            .show();
    }
    public boolean uiBusy(){return worker!=null;}
    public String uiStatus(){return status;}
    private String systemDriver(){return Language.s(R.string.driver_system);}
    /** Drivers kept on the device: {dir, lib, name}. The one in use is mirrored in driverDir/driverLib. */
    private java.util.List<JSONObject> storedDrivers(){
        ArrayList<JSONObject> list=new ArrayList<>();
        SharedPreferences prefs=getPreferences(0);
        try{
            org.json.JSONArray array=new org.json.JSONArray(prefs.getString("drivers","[]"));
            for(int i=0;i<array.length();i++){
                JSONObject d=array.getJSONObject(i);
                if(new File(d.getString("dir"),d.getString("lib")).isFile())list.add(d);
            }
            String dir=prefs.getString("driverDir",""),lib=prefs.getString("driverLib","");
            if(!dir.isEmpty()&&new File(dir,lib).isFile()&&list.stream().noneMatch(d->d.optString("dir").equals(dir))){
                list.add(new JSONObject().put("dir",dir).put("lib",lib).put("name",lib));
                saveDrivers(list);
            }
        }catch(Exception ignored){}
        return list;
    }
    private void saveDrivers(java.util.List<JSONObject> list){
        getPreferences(0).edit().putString("drivers",new org.json.JSONArray(list).toString()).apply();
    }
    private String driverLabel(java.util.List<JSONObject> list,int index){
        String name=list.get(index).optString("name");int same=0;
        for(int i=0;i<index;i++)if(list.get(i).optString("name").equals(name))same++;
        if(list.get(index).optBoolean("bundled"))name=Language.s(R.string.driver_bundled_label,name);
        return same==0?name:name+" ("+(same+1)+")";
    }
    public java.util.List<String> uiDriverChoices(){
        ArrayList<String> out=new ArrayList<>();out.add(systemDriver());
        java.util.List<JSONObject> list=storedDrivers();
        for(int i=0;i<list.size();i++)out.add(driverLabel(list,i));
        return out;
    }
    public String uiDriverSelected(){
        String dir=getPreferences(0).getString("driverDir","");
        if(dir.isEmpty())return systemDriver();
        java.util.List<JSONObject> list=storedDrivers();
        for(int i=0;i<list.size();i++)if(list.get(i).optString("dir").equals(dir))return driverLabel(list,i);
        return systemDriver();
    }
    public void uiSelectDriver(String label){
        SharedPreferences.Editor edit=getPreferences(0).edit();
        java.util.List<JSONObject> list=storedDrivers();
        boolean found=false;
        for(int i=0;i<list.size()&&!found;i++)if(driverLabel(list,i).equals(label)){
            edit.putString("driverDir",list.get(i).optString("dir")).putString("driverLib",list.get(i).optString("lib"));found=true;
        }
        if(!found)edit.remove("driverDir").remove("driverLib");
        edit.putBoolean(BundledDriver.PREF_CHOSEN,true).apply();refresh();
    }
    private final java.util.Map<String,Boolean> turnipCache=new java.util.HashMap<>();
    /** True only when the selected driver is Turnip: its library carries the TU_DEBUG option table, which no other driver has. */
    public boolean uiDriverIsTurnip(){
        String dir=getPreferences(0).getString("driverDir",""),lib=getPreferences(0).getString("driverLib","");
        if(dir.isEmpty()||lib.isEmpty())return false;
        if(BundledDriver.isBundledDir(dir))return true;  // known Turnip: skip scanning 14 MB
        File so=new File(dir,lib);
        if(!so.isFile())return false;
        String key=so.getAbsolutePath()+":"+so.length()+":"+so.lastModified();
        Boolean known=turnipCache.get(key);
        if(known!=null)return known;
        boolean found=containsAscii(so,"TU_DEBUG");
        turnipCache.put(key,found);
        return found;
    }
    private static boolean containsAscii(File file,String needle){
        byte[] pattern=needle.getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        try(java.io.InputStream in=new java.io.BufferedInputStream(new java.io.FileInputStream(file),1<<16)){
            int matched=0,b;
            while((b=in.read())!=-1){
                if(b==pattern[matched]){if(++matched==pattern.length)return true;}
                else matched=b==pattern[0]?1:0;
            }
        }catch(IOException ignored){}
        return false;
    }
    /** True for a driver the user imported; the bundled one is part of the app and cannot be deleted. */
    public boolean uiDriverStored(){
        String dir=getPreferences(0).getString("driverDir","");
        return !dir.isEmpty()&&!BundledDriver.isBundledDir(dir);
    }
    /** Removes the selected stored driver from the device and falls back to the system one. */
    public void uiDeleteDriver(){
        String dir=getPreferences(0).getString("driverDir","");
        if(dir.isEmpty()||worker!=null)return;
        java.util.List<JSONObject> list=storedDrivers();
        list.removeIf(d->d.optString("dir").equals(dir));
        saveDrivers(list);
        // Back to the default: the bundled driver on Adreno, the system one elsewhere.
        getPreferences(0).edit().remove("driverDir").remove("driverLib").putBoolean(BundledDriver.PREF_CHOSEN,false).apply();
        BundledDriver.sync(this,getPreferences(0));
        try{GameFiles.removeTree(new File(dir));}catch(IOException e){toast(Language.s(R.string.driver_remove_failed,e.getMessage()));}
        refresh();
    }
    public boolean uiAutoStart(){return getPreferences(0).getBoolean("autoStart",false);}
    public void uiSetAutoStart(boolean value){getPreferences(0).edit().putBoolean("autoStart",value).apply();}
    public boolean uiShowControls(){return getPreferences(0).getBoolean("showControls",true);}
    public void uiSetShowControls(boolean value){getPreferences(0).edit().putBoolean("showControls",value).apply();}
    public boolean uiTempEnabled(){return getPreferences(0).getBoolean("temp",false);}
    public void uiSetTemp(boolean value){getPreferences(0).edit().putBoolean("temp",value).apply();}
    /** Names of the screen layout profiles ("default" is the built-in one) and the one in use. */
    public java.util.List<String> uiLayoutNames(){return HudLayout.names(this);}
    public String uiLayoutSelected(){return HudLayout.selectedName(this);}
    public void uiSelectLayout(String name){
        try{HudLayout.select(this,name);}catch(IOException e){Toast.makeText(this,e.getMessage(),Toast.LENGTH_LONG).show();}
    }
    public void uiOpenLayoutEditor(){if(worker==null)startActivity(new Intent(this,LayoutEditorActivity.class));}
    public boolean uiFpsEnabled(){return getPreferences(0).getBoolean("fps",false);}
    public void uiSetFps(boolean value){getPreferences(0).edit().putBoolean("fps",value).apply();refresh();}
    public boolean uiOneUiEnabled(){return getPreferences(0).getBoolean("oneui",true);}
    public void uiSetOneUi(boolean value){getPreferences(0).edit().putBoolean("oneui",value).apply();refresh();}
    public String uiTuMode(){return getPreferences(0).getString("tuDebug","");}
    public void uiSetTuMode(String value){getPreferences(0).edit().putString("tuDebug",value).apply();refresh();}
    public String uiIr3Mode(){return getPreferences(0).getString("ir3Debug","");}
    public void uiSetIr3Mode(String value){getPreferences(0).edit().putString("ir3Debug",value).apply();refresh();}
    public String uiRuntimeOption(String key,String fallback){return RuntimeOptions.get(this,key,fallback);}
    public void uiSetRuntimeOption(String key,String value){
        try { RuntimeOptions.set(this,key,value); }
        catch(IOException e){toast(Language.s(R.string.toml_save_failed,e.getMessage()));}
    }
    /** Asks where to put a zip with every photo of the game; the write happens in onActivityResult. */
    public void uiExportPhotos(){
        if(worker!=null)return;
        Intent i=new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE);
        i.putExtra(Intent.EXTRA_TITLE,"renut-fotos.zip");startActivityForResult(i,PHOTO_EXPORT);
    }
    public java.util.List<String> uiSaves()throws IOException{return SaveArchive.list(new File(getFilesDir(),"runtime-user"));}
    private void pick(int code){startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE),code);}
    private void clearShaderCache(){
        File root=getFilesDir();
        try{
            // plume-pipelines.bin, its .delta.bin (what was compiled since), variants and temporaries.
            boolean removed=false;
            File[] files=root.listFiles((dir,name)->name.startsWith("plume-pipelines"));
            if(files!=null)for(File file:files)removed=Files.deleteIfExists(file.toPath())||removed;
            toast(Language.s(removed?R.string.shader_cache_cleared:R.string.shader_cache_none));
        }catch(IOException e){toast(Language.s(R.string.shader_cache_failed,e.getMessage()));}
    }
    @Override protected void onResume(){super.onResume();refresh();}
    /** Shows the last runtime report as the status while no import is running. */
    private void refresh(){
        File runtimeReport=new File(getFilesDir(),"runtime-report.txt");
        try{if(runtimeReport.isFile()&&worker==null)status=new String(Files.readAllBytes(runtimeReport.toPath()),java.nio.charset.StandardCharsets.UTF_8);}catch(IOException ignored){}
    }
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);
        if(result!=RESULT_OK||data==null||data.getData()==null)return;
        Uri uri=data.getData();
        if(request==SAVE_EXPORT||request==SAVE_IMPORT){
            if(worker!=null)return;
            final boolean importing=request==SAVE_IMPORT;
            if(importing){
                new AlertDialog.Builder(this).setTitle(Language.s(R.string.saves_import_title))
                    .setMessage(Language.s(R.string.saves_import_message))
                    .setNegativeButton(Language.s(R.string.common_cancel),null)
                    .setPositiveButton(Language.s(R.string.saves_import_action),(dialog,which)->transferSaves(uri,true)).show();
            }else transferSaves(uri,false);
            return;
        }
        if(request==PHOTO_EXPORT){
            if(worker!=null)return;
            worker=new Thread(()->{
                try{
                    int count;
                    try(OutputStream out=getContentResolver().openOutputStream(uri)){
                        if(out==null)throw new IOException(Language.s(R.string.unavailable_dest));
                        count=PhotoAlbum.exportZip(getFilesDir(),out);
                    }
                    if(count==0)try{getContentResolver().delete(uri,null,null);}catch(Exception ignored){}
                    runOnUiThread(()->Toast.makeText(this,Language.s(count>0?R.string.photos_exported:R.string.photos_export_none),Toast.LENGTH_LONG).show());
                }catch(Exception e){runOnUiThread(()->new AlertDialog.Builder(this).setTitle(Language.s(R.string.saves_failed_title)).setMessage(e.getMessage()).setPositiveButton(Language.s(R.string.common_ok),null).show());}
                finally{runOnUiThread(()->{worker=null;});}
            },"renut-photos");worker.start();
            return;
        }
        if(worker!=null||request!=DRIVER)return;
        worker=new Thread(()->{
            File stage=new File(getFilesDir(),"import-"+UUID.randomUUID());
            boolean committed=false;
            try{
                if(!stage.mkdir())throw new IOException(Language.s(R.string.imp_stage_fail));
                importDriver(uri,stage);committed=true;
                toast(Language.s(R.string.driver_import_done));
            }catch(Exception e){toast(Language.s(R.string.driver_import_failed,String.valueOf(e.getMessage())));}
            finally{
                if(!committed)try{GameFiles.removeTree(stage);}catch(IOException ignored){}
                runOnUiThread(()->{worker=null;refresh();});
            }
        },"renut-import");worker.start();
    }
    private void transferSaves(Uri uri,boolean importing){
        worker=new Thread(()->{
            try{
                File saveRoot=new File(getFilesDir(),"runtime-user");
                if(importing){
                    try(InputStream in=getContentResolver().openInputStream(uri)){
                        if(in==null)throw new IOException(Language.s(R.string.file_unavailable));
                        SaveArchive.importZip(saveRoot,in);
                    }
                }else{
                    try(OutputStream out=getContentResolver().openOutputStream(uri)){
                        if(out==null)throw new IOException(Language.s(R.string.unavailable_dest));
                        SaveArchive.exportZip(saveRoot,out);
                    }
                }
                runOnUiThread(()->Toast.makeText(this,Language.s(importing?R.string.saves_imported:R.string.saves_exported),Toast.LENGTH_LONG).show());
            }catch(Exception e){runOnUiThread(()->new AlertDialog.Builder(this).setTitle(Language.s(R.string.saves_failed_title)).setMessage(e.getMessage()).setPositiveButton(Language.s(R.string.common_ok),null).show());}
            finally{runOnUiThread(()->{worker=null;});}
        },"renut-saves");worker.start();
    }
    private void update(String message,long done,long total)throws IOException{
        if(Thread.currentThread().isInterrupted())throw new InterruptedIOException(Language.s(R.string.import_cancelled));
        long now=SystemClock.elapsedRealtime();if(now-lastProgress<250)return;lastProgress=now;
        status=message;
    }
    private void importDriver(Uri source,File stage)throws Exception{
        long total=0;int files=0;Set<String> names=new HashSet<>();
        try(InputStream input=getContentResolver().openInputStream(source);ZipInputStream zip=new ZipInputStream(Objects.requireNonNull(input))){
            ZipEntry e;byte[] buffer=new byte[65536];
            while((e=zip.getNextEntry())!=null){
                String name=e.getName();
                if(name.contains("/")||name.contains("\\")||name.equals("..")||name.contains(":")||!names.add(name)||++files>32)
                    throw new IOException(Language.s(R.string.driver_invalid_pack));
                File out=new File(stage,name);
                if(!out.getCanonicalPath().startsWith(stage.getCanonicalPath()+File.separator))throw new IOException(Language.s(R.string.driver_zip_path));
                try(OutputStream stream=new FileOutputStream(out)){
                    int n;while((n=zip.read(buffer))!=-1){total+=n;if(total>256L*1024*1024)throw new IOException(Language.s(R.string.driver_too_big));stream.write(buffer,0,n);update(Language.s(R.string.driver_importing),0,0);}
                }
            }
        }
        File meta=new File(stage,"meta.json");if(meta.length()>65536)throw new IOException(Language.s(R.string.driver_meta_big));
        JSONObject json=new JSONObject(new String(Files.readAllBytes(meta.toPath()),java.nio.charset.StandardCharsets.UTF_8));
        String lib=json.getString("libraryName");
        if(!names.contains(lib)||!lib.endsWith(".so"))throw new IOException(Language.s(R.string.driver_lib_missing));
        if(json.optInt("minApi",28)>Build.VERSION.SDK_INT)throw new IOException(Language.s(R.string.driver_needs_android));
        try(RandomAccessFile elf=new RandomAccessFile(new File(stage,lib),"r")){
            byte[] h=new byte[20];elf.readFully(h);
            if(h[0]!=127||h[1]!='E'||h[2]!='L'||h[3]!='F'||h[4]!=2||h[5]!=1||(h[18]&255)!=183||h[19]!=0)
                throw new IOException(Language.s(R.string.driver_not_arm64));
        }
        if(Thread.currentThread().isInterrupted())throw new InterruptedIOException(Language.s(R.string.import_cancelled));
        java.util.List<JSONObject> list=storedDrivers();
        list.add(new JSONObject().put("dir",stage.getAbsolutePath()).put("lib",lib).put("name",json.optString("name",lib).trim().isEmpty()?lib:json.optString("name",lib).trim()));
        if(!getPreferences(0).edit().putString("drivers",new org.json.JSONArray(list).toString())
            .putString("driverDir",stage.getAbsolutePath()).putString("driverLib",lib).putBoolean(BundledDriver.PREF_CHOSEN,true).commit())throw new IOException(Language.s(R.string.driver_save_failed));
    }
    @Override protected void onDestroy(){Thread t=worker;if(t!=null)t.interrupt();super.onDestroy();}
}
