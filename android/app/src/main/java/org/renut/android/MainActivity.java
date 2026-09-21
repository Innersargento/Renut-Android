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

public final class MainActivity extends Activity {
    private static final int ISO=10, XEX=11, DRIVER=12, EXPORT=13;
    private final int ink=Color.rgb(226,237,235), muted=Color.rgb(158,182,181);
    private static final String[] TU_DEBUG={"","perf","sysmem","gmem"};
    private TextView status, library, driver, renderer, fpsCounter, oneUiFix, tuDebug;
    private ProgressBar progress;
    private Button cancel;
    private volatile Thread worker;
    private long lastProgress;
    private String report="Nenhum teste gráfico executado.";
    private long reportReadAt;
    private final ArrayList<Button> actions=new ArrayList<>();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.rgb(12,23,28));
        getWindow().setNavigationBarColor(Color.rgb(12,23,28));
        ScrollView scroll=new ScrollView(this);
        LinearLayout body=new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(dp(24),dp(28),dp(24),dp(28));
        body.setBackgroundColor(Color.rgb(12,23,28)); scroll.addView(body); setContentView(scroll);
        body.setOnApplyWindowInsetsListener((v,insets)->{
            v.setPadding(dp(24),dp(28)+insets.getSystemWindowInsetTop(),dp(24),dp(28)+insets.getSystemWindowInsetBottom()); return insets;
        });
        body.addView(text("RENUT  /  ANDROID",13,Color.rgb(103,223,174)));
        TextView title=text("Uma nova casa\npara a aventura.",34,ink); title.setTypeface(null,Typeface.BOLD); body.addView(title);
        body.addView(text("Prévia de desenvolvimento · 0.4\nImporte seus arquivos e teste a inicialização do Banjo com gráficos nativos Vulkan. Compatibilidade e desempenho estão em desenvolvimento.",15,muted));
        LinearLayout game=card(body,"01   SEU JOGO");
        library=text("Nenhum arquivo importado",16,ink); game.addView(library);
        button(game,"Importar imagem ISO",()->pick(ISO));
        button(game,"Importar apenas o XEX",()->pick(XEX));
        button(game,"Iniciar Banjo (experimental)",()->{
            Intent i=new Intent(this,RuntimeActivity.class).putExtra("launch",true);
            i.putExtra("driverDir",getPreferences(0).getString("driverDir",""));
            i.putExtra("driverLib",getPreferences(0).getString("driverLib",""));
            i.putExtra("renderer",getPreferences(0).getString("renderer","xenos"));
            i.putExtra("fps",getPreferences(0).getBoolean("fps",false));
            i.putExtra("oneUiFix",getPreferences(0).getBoolean("oneui",true));
            i.putExtra("tuDebug",getPreferences(0).getString("tuDebug",""));startActivity(i);
        });
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
        renderer=text("",16,ink); graphics.addView(renderer);
        button(graphics,"Alternar renderizador",()->{
            String next="plume".equals(getPreferences(0).getString("renderer","xenos"))?"xenos":"plume";
            getPreferences(0).edit().putString("renderer",next).apply(); refresh();
        });
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
        button(graphics,"Alternar TU_DEBUG",()->{
            int next=(Arrays.asList(TU_DEBUG).indexOf(getPreferences(0).getString("tuDebug",""))+1)%TU_DEBUG.length;
            getPreferences(0).edit().putString("tuDebug",TU_DEBUG[next]).apply(); refresh();
        });
        graphics.addView(text("Depuração do Turnip. \"perf\" registra no logcat por que cada render pass foi para GMEM ou sysmem; \"sysmem\" e \"gmem\" forçam um dos modos. Só para medição. Não tem efeito no driver do sistema.",13,muted));
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
        refresh();
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
    }
    private void pick(int code){startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE),code);}
    @Override protected void onResume(){super.onResume();refresh();}
    private void refresh(){
        if(library==null)return;
        File active=new File(getFilesDir(),"active-game.txt");
        try{
            String name=new String(Files.readAllBytes(active.toPath()),java.nio.charset.StandardCharsets.UTF_8).trim();
            File folder=new File(getFilesDir(),name);
            boolean full=new File(folder,"Bundle").isDirectory();
            library.setText("Banjo-Kazooie: Nuts & Bolts\nEuropa · "+(full?"assets presentes":"XEX identificado; importe a ISO para obter os assets")+"\nInicialização experimental disponível");
        }catch(IOException e){library.setText("Nenhum arquivo importado");}
        driver.setText(getPreferences(0).getString("driverLib","Driver do sistema"));
        renderer.setText("plume".equals(getPreferences(0).getString("renderer","xenos"))
            ?"Renderizador: Plume nativo (em desenvolvimento)":"Renderizador: RexGlue (emulação Xenos)");
        fpsCounter.setText(getPreferences(0).getBoolean("fps",false)?"Contador de FPS: ativado":"Contador de FPS: desativado");
        oneUiFix.setText(getPreferences(0).getBoolean("oneui",true)?"One UI fix: ativado":"One UI fix: desativado");
        String debug=getPreferences(0).getString("tuDebug","");
        tuDebug.setText("TU_DEBUG: "+(debug.isEmpty()?"desativado":debug));
        File diagnostic=new File(getFilesDir(),"probe-report.txt");
        try{if(diagnostic.lastModified()>reportReadAt){report=new String(Files.readAllBytes(diagnostic.toPath()),java.nio.charset.StandardCharsets.UTF_8);reportReadAt=diagnostic.lastModified();if(worker==null)status.setText(report);}}catch(IOException ignored){}
        File runtimeReport=new File(getFilesDir(),"runtime-report.txt");
        try{if(runtimeReport.isFile()&&worker==null)status.setText(new String(Files.readAllBytes(runtimeReport.toPath()),java.nio.charset.StandardCharsets.UTF_8));}catch(IOException ignored){}
    }
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);
        if(result!=RESULT_OK||data==null||data.getData()==null)return;
        Uri uri=data.getData();
        if(request==EXPORT){
            try(OutputStream out=getContentResolver().openOutputStream(uri)){
                if(out==null)throw new IOException("Destino indisponível.");
                String text="Aparelho: "+Build.MANUFACTURER+" "+Build.MODEL+"\nAndroid: "+Build.VERSION.RELEASE+"\nABI: "+Arrays.toString(Build.SUPPORTED_ABIS)+"\n"+report;
                File runtimeReport=new File(getFilesDir(),"runtime-report.txt");
                if(runtimeReport.isFile())text+="\n\n"+new String(Files.readAllBytes(runtimeReport.toPath()),java.nio.charset.StandardCharsets.UTF_8);
                out.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }catch(IOException e){status.setText(e.getMessage());}return;
        }
        if(worker!=null)return;
        for(Button b:actions)b.setEnabled(false);cancel.setVisibility(View.VISIBLE);progress.setVisibility(View.VISIBLE);progress.setIndeterminate(true);
        worker=new Thread(()->{
            File stage=new File(getFilesDir(),"import-"+UUID.randomUUID());
            boolean committed=false;
            try{
                if(!stage.mkdir())throw new IOException("Não foi possível criar a pasta de importação.");
                if(request==DRIVER){
                    importDriver(uri,stage);committed=true;
                }else{
                    File gameData=new File(stage,"data");
                    if(!gameData.mkdir())throw new IOException("Não foi possível criar a pasta de dados.");
                    File xex;
                    if(request==ISO){
                        File iso=new File(stage,"source.iso");
                        update("Copiando imagem para leitura. É necessário espaço para a ISO e os arquivos extraídos.",0,0);
                        copy(uri,iso,16L*1024*1024*1024);
                        try(RandomAccessFile input=new RandomAccessFile(iso,"r")){
                            DiscImage disc=new DiscImage(input);
                            DiscImage.Entry executable=disc.entries().stream().filter(e->e.path.equalsIgnoreCase("default.xex")).findFirst().orElseThrow(()->new IOException("XEX ausente."));
                            File check=new File(stage,"check.xex");
                            try(OutputStream out=new FileOutputStream(check)){
                                if(executable.size>256L*1024*1024)throw new IOException("XEX excessivamente grande.");
                                input.seek(executable.offset);byte[] block=new byte[1024*1024];long remaining=executable.size;
                                while(remaining>0){int n=(int)Math.min(block.length,remaining);input.readFully(block,0,n);out.write(block,0,n);remaining-=n;}
                            }
                            GameFiles.requireEuropean(check);Files.delete(check.toPath());
                            disc.extract(gameData,(done,total)->update("Extraindo arquivos do jogo…",done,total));
                        }
                        Files.delete(iso.toPath());
                        xex=Arrays.stream(Objects.requireNonNull(gameData.listFiles())).filter(f->f.getName().equalsIgnoreCase("default.xex")).findFirst().orElseThrow(()->new IOException("XEX ausente."));
                        if(!new File(gameData,"full-disc.marker").createNewFile())throw new IOException("Falha ao registrar importação.");
                    }else{xex=new File(gameData,"default.xex");copy(uri,xex,256L*1024*1024);}
                    GameFiles.requireEuropean(xex);
                    if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Importação cancelada.");
                    GameFiles.atomicWrite(new File(getFilesDir(),"active-game.txt"),stage.getName()+"/data");committed=true;
                }
                runOnUiThread(()->status.setText("Importação concluída e validada."));
            }catch(Exception e){runOnUiThread(()->status.setText("Importação não concluída: "+e.getMessage()));}
            finally{
                if(!committed)try{GameFiles.removeTree(stage);}catch(IOException ignored){}
                runOnUiThread(()->{worker=null;cancel.setVisibility(View.GONE);progress.setVisibility(View.GONE);for(Button b:actions)b.setEnabled(true);refresh();});
            }
        },"renut-import");worker.start();
    }
    private void copy(Uri source,File target,long limit)throws IOException{
        try(InputStream in=getContentResolver().openInputStream(source);OutputStream out=new FileOutputStream(target)){
            if(in==null)throw new IOException("Arquivo inacessível.");
            byte[] buffer=new byte[1024*1024];long total=0;int count;
            while((count=in.read(buffer))!=-1){
                if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Importação cancelada.");
                total+=count;if(total>limit)throw new IOException("Arquivo excede o limite de importação.");
                if(target.getUsableSpace()<count+64L*1024*1024)throw new IOException("Espaço livre insuficiente.");
                out.write(buffer,0,count);update("Copiados "+total/(1024*1024)+" MB",0,0);
            }
        }
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
        if(!getPreferences(0).edit().putString("driverDir",stage.getAbsolutePath()).putString("driverLib",lib).commit())throw new IOException("Não foi possível salvar o driver.");
    }
    @Override protected void onDestroy(){Thread t=worker;if(t!=null)t.interrupt();super.onDestroy();}
}
