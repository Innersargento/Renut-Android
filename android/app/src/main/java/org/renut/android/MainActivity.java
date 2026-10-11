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
    private static final int DRIVER=12, EXPORT=13, SAVE_IMPORT=14, SAVE_EXPORT=15;
    private final int ink=Color.rgb(226,237,235), muted=Color.rgb(158,182,181);
    private TextView status, library, driver, fpsCounter, oneUiFix, tuDebug, ir3Debug, savesSummary;
    private LinearLayout savesList;
    private ProgressBar progress;
    private Button cancel;
    private volatile Thread worker;
    private long lastProgress;
    private String report="Nenhum teste gráfico executado.";
    private long reportReadAt;
    private final ArrayList<Button> actions=new ArrayList<>();
    private final Map<String,Runnable> uiActions=new LinkedHashMap<>();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        // No game yet: the first-run screens introduce the project and install it. They hand
        // over to this launcher (a new instance) once a game is imported.
        if(!hasGame()){
            startActivity(new Intent(this,WelcomeActivity.class));
            finish();
            return;
        }
        getWindow().setStatusBarColor(Color.rgb(12,23,28));
        getWindow().setNavigationBarColor(Color.rgb(12,23,28));
        LinearLayout root=new LinearLayout(this);root.setOrientation(LinearLayout.VERTICAL);root.setBackgroundColor(Color.rgb(12,23,28));setContentView(root);
        FrameLayout pages=new FrameLayout(this);root.addView(pages,new LinearLayout.LayoutParams(-1,0,1));
        ScrollView savesPage=new ScrollView(this);pages.addView(savesPage);
        LinearLayout savesBody=new LinearLayout(this);savesBody.setOrientation(LinearLayout.VERTICAL);savesBody.setPadding(dp(24),dp(28),dp(24),dp(28));savesBody.setBackgroundColor(Color.rgb(12,23,28));savesPage.addView(savesBody);
        savesBody.addView(text("RENUT  /  SAVES",13,Color.rgb(103,223,174)));
        TextView savesTitle=text("Seus saves",34,ink);savesTitle.setTypeface(null,Typeface.BOLD);savesBody.addView(savesTitle);
        LinearLayout saveCard=card(savesBody,"ARQUIVOS DO JOGO");
        savesSummary=text("Procurando saves…",15,ink);saveCard.addView(savesSummary);
        savesList=new LinearLayout(this);savesList.setOrientation(LinearLayout.VERTICAL);saveCard.addView(savesList);
        button(saveCard,"Exportar saves (.zip)",()->{
            Intent i=new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE);
            i.putExtra(Intent.EXTRA_TITLE,"renut-saves.zip");startActivityForResult(i,SAVE_EXPORT);
        });
        button(saveCard,"Importar saves (.zip)",()->{
            Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(i,SAVE_IMPORT);
        });
        ScrollView scroll=new ScrollView(this);pages.addView(scroll);scroll.setVisibility(View.GONE);
        LinearLayout body=new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(dp(24),dp(28),dp(24),dp(28));
        body.setBackgroundColor(Color.rgb(12,23,28)); scroll.addView(body);
        body.setOnApplyWindowInsetsListener((v,insets)->{
            v.setPadding(dp(24),dp(28)+insets.getSystemWindowInsetTop(),dp(24),dp(28)+insets.getSystemWindowInsetBottom()); return insets;
        });
        body.addView(text("RENUT  /  ANDROID",13,Color.rgb(103,223,174)));
        TextView title=text("Uma nova casa\npara a aventura.",34,ink); title.setTypeface(null,Typeface.BOLD); body.addView(title);
        body.addView(text("Prévia de desenvolvimento · 0.4\nImporte seus arquivos e teste a inicialização do Banjo com gráficos nativos Vulkan. Compatibilidade e desempenho estão em desenvolvimento.",15,muted));
        LinearLayout game=card(body,"01   SEU JOGO");
        library=text("Nenhum arquivo importado",16,ink); game.addView(library);
        CheckBox showControls=new CheckBox(this);
        showControls.setText("Mostrar controles na tela");
        showControls.setTextColor(ink);
        showControls.setChecked(getPreferences(0).getBoolean("showControls",true));
        showControls.setOnCheckedChangeListener((view,checked)->
            getPreferences(0).edit().putBoolean("showControls",checked).apply());
        game.addView(showControls);
        CheckBox autoStart=new CheckBox(this);
        autoStart.setText("Iniciar direto no jogo");
        autoStart.setTextColor(ink);
        autoStart.setChecked(getPreferences(0).getBoolean("autoStart",false));
        autoStart.setOnCheckedChangeListener((view,checked)->
            getPreferences(0).edit().putBoolean("autoStart",checked).apply());
        game.addView(autoStart);
        button(game,"Configurar controle externo",()->startActivity(new Intent(this,ControllerSettingsActivity.class)));
        button(game,"Iniciar Banjo (experimental)",()->startGame());
        button(game,"Validar runtime ARM64",()->startActivity(new Intent(this,RuntimeActivity.class)));
        button(game,"Testar runtime com Plume",()->{
            Intent i=new Intent(this,RuntimeActivity.class).putExtra("graphics",true);
            i.putExtra("driverDir",getPreferences(0).getString("driverDir",""));
            i.putExtra("driverLib",getPreferences(0).getString("driverLib",""));startActivity(i);
        });
        LinearLayout graphics=card(body,"02   GRÁFICOS");
        driver=text("Driver do sistema",16,ink); graphics.addView(driver);
        button(graphics,"Importar driver Adreno (.zip)",()->pick(DRIVER));
        button(graphics,"Usar driver do sistema",()->{getPreferences(0).edit().remove("driverDir").remove("driverLib").apply(); refresh();});
        button(graphics,"Apagar cache de shaders",()->new AlertDialog.Builder(this)
            .setTitle("Apagar cache de shaders?")
            .setMessage("Remove o cache Vulkan salvo pelo Plume. Os shaders serão compilados novamente na próxima execução; seus jogos e saves serão preservados.")
            .setNegativeButton("Cancelar",null)
            .setPositiveButton("Apagar",(dialog,which)->clearShaderCache())
            .show());
        fpsCounter=text("",16,ink); graphics.addView(fpsCounter);
        button(graphics,"Ativar/desativar contador de FPS",()->{
            getPreferences(0).edit().putBoolean("fps",!getPreferences(0).getBoolean("fps",false)).apply(); refresh();
        });
        oneUiFix=text("",16,ink); graphics.addView(oneUiFix);
        button(graphics,"Ativar/desativar One UI fix",()->{
            getPreferences(0).edit().putBoolean("oneui",!getPreferences(0).getBoolean("oneui",true)).apply(); refresh();
        });
        graphics.addView(text("Pede ao Turnip a dica de flag UBWC do processador de texturas (FD_DEV_FEATURES). Sem ela, amostrar uma imagem comprimida que a GPU acabou de escrever devolve dado velho: a tela pisca preta e os quadros saem em blocos. Não tem efeito no driver do sistema.",13,muted));
        tuDebug=text("",16,ink); graphics.addView(tuDebug);
        graphics.addView(text("Depuração do Turnip. \"perf\" registra no logcat por que cada render pass foi para GMEM ou sysmem; \"sysmem\" e \"gmem\" forçam um dos modos. Só para medição. Não tem efeito no driver do sistema.",13,muted));
        ir3Debug=text("",16,ink); graphics.addView(ir3Debug);
        graphics.addView(text("Opções do compilador de shaders do Turnip. \"nopreamble\" é o mais rápido, mas faz placas penduradas piscarem entre dois tons; \"noearlypreamble\", \"nouboopt\" e \"nodescprefetch\" servem para medir. Cada combinação tem seu próprio cache de shaders. Não tem efeito no driver do sistema.",13,muted));
        button(graphics,"Testar Plume no aparelho",()->{
            Intent intent=new Intent(this,ProbeActivity.class);
            intent.putExtra("driverDir",getPreferences(0).getString("driverDir",""));
            intent.putExtra("driverLib",getPreferences(0).getString("driverLib",""));
            startActivity(intent);
        });
        graphics.addView(text("O teste mostra uma animação simples e mede CPU/GPU. Os resultados não indicam o desempenho do Banjo.",13,muted));
        LinearLayout diagnostics=card(body,"03   ATIVIDADE");
        status=text("Pronto para começar.",14,ink); diagnostics.addView(status);
        progress=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal); progress.setVisibility(View.GONE); diagnostics.addView(progress);
        cancel=new Button(this); cancel.setText("Cancelar importação"); cancel.setVisibility(View.GONE);
        cancel.setOnClickListener(v->{Thread t=worker;if(t!=null)t.interrupt();}); diagnostics.addView(cancel);
        button(diagnostics,"Exportar diagnóstico",()->{
            Intent i=new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain").addCategory(Intent.CATEGORY_OPENABLE);
            i.putExtra(Intent.EXTRA_TITLE,"renut-diagnostico.txt");startActivityForResult(i,EXPORT);
        });
        LinearLayout nav=new LinearLayout(this);nav.setPadding(dp(16),dp(8),dp(16),dp(8));root.addView(nav);
        Button savesTab=new Button(this);savesTab.setText("▣  Saves");savesTab.setAllCaps(false);
        Button settingsTab=new Button(this);settingsTab.setText("⚙  Configurações");settingsTab.setAllCaps(false);
        nav.addView(savesTab,new LinearLayout.LayoutParams(0,dp(52),1));nav.addView(settingsTab,new LinearLayout.LayoutParams(0,dp(52),1));
        savesTab.setOnClickListener(v->{savesPage.setVisibility(View.VISIBLE);scroll.setVisibility(View.GONE);refreshSaves();});
        settingsTab.setOnClickListener(v->{savesPage.setVisibility(View.GONE);scroll.setVisibility(View.VISIBLE);});
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
        i.putExtra("showControls",getPreferences(0).getBoolean("showControls",true));
        boolean turnip=uiDriverIsTurnip();
        i.putExtra("oneUiFix",turnip&&getPreferences(0).getBoolean("oneui",true));
        i.putExtra("sceneMsaa",false);
        i.putExtra("tuDebug",turnip?getPreferences(0).getString("tuDebug",""):"");
        i.putExtra("ir3Debug",turnip?getPreferences(0).getString("ir3Debug",""):"");
        startActivity(i);
    }
    private int dp(int n){return (int)(getResources().getDisplayMetrics().density*n);}
    private TextView text(String value,int size,int color){TextView t=new TextView(this);t.setText(value);t.setTextSize(size);t.setTextColor(color);t.setPadding(0,dp(7),0,dp(9));return t;}
    private LinearLayout card(LinearLayout parent,String label){
        LinearLayout box=new LinearLayout(this);box.setOrientation(LinearLayout.VERTICAL);box.setPadding(dp(18),dp(14),dp(18),dp(14));
        GradientDrawable bg=new GradientDrawable();bg.setColor(Color.rgb(22,39,46));bg.setCornerRadius(dp(18));box.setBackground(bg);
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.topMargin=dp(18);parent.addView(box,lp);
        box.addView(text(label,12,Color.rgb(103,223,174)));return box;
    }
    private void button(LinearLayout box,String name,Runnable action){
        Button b=new Button(this);b.setText(name);b.setAllCaps(false);b.setTextColor(ink);
        b.setOnClickListener(v->action.run());box.addView(b,new LinearLayout.LayoutParams(-1,dp(54)));actions.add(b);
        uiActions.put(name,action);
    }
    public void uiAction(String name){Runnable action=uiActions.get(name);if(action!=null&&worker==null)action.run();}
    public boolean uiBusy(){return worker!=null;}
    public boolean uiCanCancel(){return worker!=null&&cancel.getVisibility()==View.VISIBLE;}
    public void uiCancelImport(){Thread t=worker;if(t!=null&&uiCanCancel())t.interrupt();}
    public String uiLibrary(){return library.getText().toString();}
    public String uiStatus(){return status.getText().toString();}
    public String uiDriver(){return driver.getText().toString();}
    private static final String SYSTEM_DRIVER="Driver do sistema";
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
        return same==0?name:name+" ("+(same+1)+")";
    }
    public java.util.List<String> uiDriverChoices(){
        ArrayList<String> out=new ArrayList<>();out.add(SYSTEM_DRIVER);
        java.util.List<JSONObject> list=storedDrivers();
        for(int i=0;i<list.size();i++)out.add(driverLabel(list,i));
        return out;
    }
    public String uiDriverSelected(){
        String dir=getPreferences(0).getString("driverDir","");
        if(dir.isEmpty())return SYSTEM_DRIVER;
        java.util.List<JSONObject> list=storedDrivers();
        for(int i=0;i<list.size();i++)if(list.get(i).optString("dir").equals(dir))return driverLabel(list,i);
        return SYSTEM_DRIVER;
    }
    public void uiSelectDriver(String label){
        SharedPreferences.Editor edit=getPreferences(0).edit();
        java.util.List<JSONObject> list=storedDrivers();
        boolean found=false;
        for(int i=0;i<list.size()&&!found;i++)if(driverLabel(list,i).equals(label)){
            edit.putString("driverDir",list.get(i).optString("dir")).putString("driverLib",list.get(i).optString("lib"));found=true;
        }
        if(!found)edit.remove("driverDir").remove("driverLib");
        edit.apply();refresh();
    }
    private final java.util.Map<String,Boolean> turnipCache=new java.util.HashMap<>();
    /** True only when the selected driver is Turnip: its library carries the TU_DEBUG option table, which no other driver has. */
    public boolean uiDriverIsTurnip(){
        String dir=getPreferences(0).getString("driverDir",""),lib=getPreferences(0).getString("driverLib","");
        if(dir.isEmpty()||lib.isEmpty())return false;
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
    public boolean uiDriverStored(){return !getPreferences(0).getString("driverDir","").isEmpty();}
    /** Removes the selected stored driver from the device and falls back to the system one. */
    public void uiDeleteDriver(){
        String dir=getPreferences(0).getString("driverDir","");
        if(dir.isEmpty()||worker!=null)return;
        java.util.List<JSONObject> list=storedDrivers();
        list.removeIf(d->d.optString("dir").equals(dir));
        saveDrivers(list);
        getPreferences(0).edit().remove("driverDir").remove("driverLib").apply();
        try{GameFiles.removeTree(new File(dir));}catch(IOException e){status.setText("Driver removido da lista, mas os arquivos não foram apagados: "+e.getMessage());}
        refresh();
    }
    public String uiFps(){return fpsCounter.getText().toString();}
    public String uiOneUi(){return oneUiFix.getText().toString();}
    public String uiTu(){return tuDebug.getText().toString();}
    public String uiIr3(){return ir3Debug.getText().toString();}
    public boolean uiAutoStart(){return getPreferences(0).getBoolean("autoStart",false);}
    public void uiSetAutoStart(boolean value){getPreferences(0).edit().putBoolean("autoStart",value).apply();}
    public boolean uiShowControls(){return getPreferences(0).getBoolean("showControls",true);}
    public void uiSetShowControls(boolean value){getPreferences(0).edit().putBoolean("showControls",value).apply();}
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
        catch(IOException e){status.setText("Não foi possível salvar renut.toml: "+e.getMessage());}
    }
    public java.util.List<String> uiSaves()throws IOException{return SaveArchive.list(new File(getFilesDir(),"runtime-user"));}
    private void pick(int code){startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE),code);}
    private void refreshSaves(){
        if(savesList==null)return;
        savesList.removeAllViews();
        try{
            java.util.List<String> entries=SaveArchive.list(new File(getFilesDir(),"runtime-user"));
            savesSummary.setText(entries.isEmpty()?"Nenhum save encontrado neste aparelho.":entries.size()+" arquivo(s) de save encontrado(s).");
            for(String name:entries)savesList.addView(text(name,13,muted));
        }catch(IOException e){savesSummary.setText("Não foi possível ler os saves: "+e.getMessage());}
    }
    private void clearShaderCache(){
        File root=getFilesDir();
        try{
            // plume-pipelines.bin, its .delta.bin (what was compiled since), variants and temporaries.
            boolean removed=false;
            File[] files=root.listFiles((dir,name)->name.startsWith("plume-pipelines"));
            if(files!=null)for(File file:files)removed=Files.deleteIfExists(file.toPath())||removed;
            status.setText(removed?"Cache de shaders apagado. Será recriado na próxima execução.":"Nenhum cache de shaders salvo para apagar.");
        }catch(IOException e){status.setText("Não foi possível apagar o cache de shaders: "+e.getMessage());}
    }
    @Override protected void onResume(){super.onResume();refresh();refreshSaves();}
    private void refresh(){
        if(library==null)return;
        File active=new File(getFilesDir(),"active-game.txt");
        try{
            String name=new String(Files.readAllBytes(active.toPath()),java.nio.charset.StandardCharsets.UTF_8).trim();
            File folder=new File(getFilesDir(),name);
            boolean full=new File(folder,"Bundle").isDirectory();
            library.setText("Banjo-Kazooie: Nuts & Bolts\n"+(full?"assets presentes":"XEX identificado; importe a ISO para obter os assets")+"\nInicialização experimental disponível");
        }catch(IOException e){library.setText("Nenhum arquivo importado");}
        driver.setText(uiDriverSelected());
        fpsCounter.setText(getPreferences(0).getBoolean("fps",false)?"Contador de FPS: ativado":"Contador de FPS: desativado");
        oneUiFix.setText(getPreferences(0).getBoolean("oneui",true)?"One UI fix: ativado":"One UI fix: desativado");
        String debug=getPreferences(0).getString("tuDebug","");
        tuDebug.setText("TU_DEBUG: "+(debug.isEmpty()?"desativado":debug));
        String ir3=getPreferences(0).getString("ir3Debug","");
        ir3Debug.setText("IR3_SHADER_DEBUG: "+(ir3.isEmpty()?"desativado":ir3));
        File diagnostic=new File(getFilesDir(),"probe-report.txt");
        try{if(diagnostic.lastModified()>reportReadAt){report=new String(Files.readAllBytes(diagnostic.toPath()),java.nio.charset.StandardCharsets.UTF_8);reportReadAt=diagnostic.lastModified();if(worker==null)status.setText(report);}}catch(IOException ignored){}
        File runtimeReport=new File(getFilesDir(),"runtime-report.txt");
        try{if(runtimeReport.isFile()&&worker==null)status.setText(new String(Files.readAllBytes(runtimeReport.toPath()),java.nio.charset.StandardCharsets.UTF_8));}catch(IOException ignored){}
    }
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);
        if(result!=RESULT_OK||data==null||data.getData()==null)return;
        Uri uri=data.getData();
        if(request==SAVE_EXPORT||request==SAVE_IMPORT){
            if(worker!=null)return;
            final boolean importing=request==SAVE_IMPORT;
            if(importing){
                new AlertDialog.Builder(this).setTitle("Importar saves?")
                    .setMessage("Arquivos com o mesmo caminho serão substituídos. Feche o jogo antes de continuar.")
                    .setNegativeButton("Cancelar",null)
                    .setPositiveButton("Importar",(dialog,which)->transferSaves(uri,true)).show();
            }else transferSaves(uri,false);
            return;
        }
        if(request==EXPORT){
            try(OutputStream out=getContentResolver().openOutputStream(uri)){
                if(out==null)throw new IOException("Destino indisponível.");
                String text="Aparelho: "+Build.MANUFACTURER+" "+Build.MODEL+"\nAndroid: "+Build.VERSION.RELEASE+"\nABI: "+Arrays.toString(Build.SUPPORTED_ABIS)+"\n"+report;
                File runtimeReport=new File(getFilesDir(),"runtime-report.txt");
                if(runtimeReport.isFile())text+="\n\n"+new String(Files.readAllBytes(runtimeReport.toPath()),java.nio.charset.StandardCharsets.UTF_8);
                out.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }catch(IOException e){status.setText(e.getMessage());}return;
        }
        if(worker!=null||request!=DRIVER)return;
        for(Button b:actions)b.setEnabled(false);cancel.setVisibility(View.VISIBLE);progress.setVisibility(View.VISIBLE);progress.setIndeterminate(true);
        worker=new Thread(()->{
            File stage=new File(getFilesDir(),"import-"+UUID.randomUUID());
            boolean committed=false;
            try{
                if(!stage.mkdir())throw new IOException("Não foi possível criar a pasta de importação.");
                importDriver(uri,stage);committed=true;
                runOnUiThread(()->status.setText("Importação concluída e validada."));
            }catch(Exception e){runOnUiThread(()->status.setText("Importação não concluída: "+e.getMessage()));}
            finally{
                if(!committed)try{GameFiles.removeTree(stage);}catch(IOException ignored){}
                runOnUiThread(()->{worker=null;cancel.setVisibility(View.GONE);progress.setVisibility(View.GONE);for(Button b:actions)b.setEnabled(true);refresh();});
            }
        },"renut-import");worker.start();
    }
    private void transferSaves(Uri uri,boolean importing){
        for(Button b:actions)b.setEnabled(false);
        worker=new Thread(()->{
            try{
                File saveRoot=new File(getFilesDir(),"runtime-user");
                if(importing){
                    try(InputStream in=getContentResolver().openInputStream(uri)){
                        if(in==null)throw new IOException("Arquivo indisponível.");
                        SaveArchive.importZip(saveRoot,in);
                    }
                }else{
                    try(OutputStream out=getContentResolver().openOutputStream(uri)){
                        if(out==null)throw new IOException("Destino indisponível.");
                        SaveArchive.exportZip(saveRoot,out);
                    }
                }
                runOnUiThread(()->Toast.makeText(this,importing?"Saves importados.":"Saves exportados.",Toast.LENGTH_LONG).show());
            }catch(Exception e){runOnUiThread(()->new AlertDialog.Builder(this).setTitle("Falha nos saves").setMessage(e.getMessage()).setPositiveButton("OK",null).show());}
            finally{runOnUiThread(()->{worker=null;for(Button b:actions)b.setEnabled(true);refreshSaves();});}
        },"renut-saves");worker.start();
    }
    private void update(String message,long done,long total)throws IOException{
        if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Importação cancelada.");
        long now=SystemClock.elapsedRealtime();if(now-lastProgress<250)return;lastProgress=now;
        runOnUiThread(()->{status.setText(message);progress.setIndeterminate(total==0);if(total>0)progress.setProgress((int)(done*100/total));});
    }
    private void importDriver(Uri source,File stage)throws Exception{
        long total=0;int files=0;Set<String> names=new HashSet<>();
        try(InputStream input=getContentResolver().openInputStream(source);ZipInputStream zip=new ZipInputStream(Objects.requireNonNull(input))){
            ZipEntry e;byte[] buffer=new byte[65536];
            while((e=zip.getNextEntry())!=null){
                String name=e.getName();
                if(name.contains("/")||name.contains("\\")||name.equals("..")||name.contains(":")||!names.add(name)||++files>32)
                    throw new IOException("Pacote de driver inválido: use um ZIP com arquivos na raiz.");
                File out=new File(stage,name);
                if(!out.getCanonicalPath().startsWith(stage.getCanonicalPath()+File.separator))throw new IOException("Caminho inválido no ZIP.");
                try(OutputStream stream=new FileOutputStream(out)){
                    int n;while((n=zip.read(buffer))!=-1){total+=n;if(total>256L*1024*1024)throw new IOException("Driver excede 256 MB.");stream.write(buffer,0,n);update("Importando driver…",0,0);}
                }
            }
        }
        File meta=new File(stage,"meta.json");if(meta.length()>65536)throw new IOException("Metadados excessivos.");
        JSONObject json=new JSONObject(new String(Files.readAllBytes(meta.toPath()),java.nio.charset.StandardCharsets.UTF_8));
        String lib=json.getString("libraryName");
        if(!names.contains(lib)||!lib.endsWith(".so"))throw new IOException("Biblioteca do driver ausente.");
        if(json.optInt("minApi",28)>Build.VERSION.SDK_INT)throw new IOException("Driver exige uma versão Android mais recente.");
        try(RandomAccessFile elf=new RandomAccessFile(new File(stage,lib),"r")){
            byte[] h=new byte[20];elf.readFully(h);
            if(h[0]!=127||h[1]!='E'||h[2]!='L'||h[3]!='F'||h[4]!=2||h[5]!=1||(h[18]&255)!=183||h[19]!=0)
                throw new IOException("O driver precisa ser uma biblioteca ELF ARM64.");
        }
        if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Importação cancelada.");
        java.util.List<JSONObject> list=storedDrivers();
        list.add(new JSONObject().put("dir",stage.getAbsolutePath()).put("lib",lib).put("name",json.optString("name",lib).trim().isEmpty()?lib:json.optString("name",lib).trim()));
        if(!getPreferences(0).edit().putString("drivers",new org.json.JSONArray(list).toString())
            .putString("driverDir",stage.getAbsolutePath()).putString("driverLib",lib).commit())throw new IOException("Não foi possível salvar o driver.");
    }
    @Override protected void onDestroy(){Thread t=worker;if(t!=null)t.interrupt();super.onDestroy();}
}
