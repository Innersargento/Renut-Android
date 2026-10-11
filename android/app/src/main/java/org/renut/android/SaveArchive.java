package org.renut.android;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Imports and exports the save locations used by RexGlue's content manager. */
final class SaveArchive {
    private static final long MAX_BYTES=512L*1024*1024;
    private static final int MAX_FILES=2000;
    private SaveArchive(){}

    static List<String> list(File root)throws IOException {
        ArrayList<String> names=new ArrayList<>();
        if(root.isDirectory())collect(root,root,names);
        Collections.sort(names);
        return names;
    }
    private static void collect(File root,File dir,List<String> names)throws IOException {
        File[] children=dir.listFiles();
        if(children==null)throw new IOException(Language.s(R.string.save_folder));
        for(File child:children){
            if(Files.isSymbolicLink(child.toPath()))continue;
            if(child.isDirectory())collect(root,child,names);
            else if(child.isFile()){
                String name=root.toPath().relativize(child.toPath()).toString().replace(File.separatorChar,'/');
                if(isSavePath(name)){
                    if(names.size()>=MAX_FILES)throw new IOException(Language.s(R.string.save_too_many));
                    names.add(name);
                }
            }
        }
    }
    private static boolean hex8(String s){return s.matches("[0-9A-Fa-f]{8}");}
    private static boolean isSavePath(String name){
        String[] p=name.split("/",-1);
        for(String part:p)if(part.isEmpty()||part.equals(".")||part.equals("..")||part.indexOf('\\')>=0||part.indexOf(':')>=0)return false;
        // Per-game profile data: title_id/profile/user_name/...
        if(p.length>=4&&hex8(p[0])&&p[1].equals("profile"))return true;
        // XContent saved-game packages and their headers: xuid/title_id/type/...
        if(p.length>=4&&hex8(p[1])&&p[2].equalsIgnoreCase("00000001"))return true;
        return p.length>=5&&hex8(p[1])&&p[2].equals("Headers")&&p[3].equalsIgnoreCase("00000001");
    }
    static void exportZip(File root,OutputStream output)throws IOException {
        List<String> names=list(root);
        if(names.isEmpty())throw new IOException(Language.s(R.string.save_none));
        long total=0;
        try(ZipOutputStream zip=new ZipOutputStream(output)){
            byte[] buffer=new byte[65536];
            for(String name:names){
                File file=new File(root,name.replace('/',File.separatorChar));
                total+=file.length();if(total>MAX_BYTES)throw new IOException(Language.s(R.string.save_size));
                zip.putNextEntry(new ZipEntry(name));
                try(InputStream in=new FileInputStream(file)){int n;while((n=in.read(buffer))!=-1)zip.write(buffer,0,n);}
                zip.closeEntry();
            }
        }
    }
    static void importZip(File root,InputStream input)throws IOException {
        File stage=new File(root.getParentFile(),"save-import-"+UUID.randomUUID());
        if(!stage.mkdir())throw new IOException(Language.s(R.string.save_prepare));
        try{
            int count=0;long total=0;byte[] buffer=new byte[65536];Set<String> names=new HashSet<>();
            try(ZipInputStream zip=new ZipInputStream(input)){
                ZipEntry entry;
                while((entry=zip.getNextEntry())!=null){
                    String name=entry.getName();
                    if(entry.isDirectory())continue;
                    if(!isSavePath(name)||!names.add(name)||++count>MAX_FILES)
                        throw new IOException(Language.s(R.string.save_zip_path));
                    File out=new File(stage,name.replace('/',File.separatorChar));
                    if(!out.getCanonicalPath().startsWith(stage.getCanonicalPath()+File.separator))
                        throw new IOException(Language.s(R.string.save_zip_path2));
                    if(!out.getParentFile().isDirectory()&&!out.getParentFile().mkdirs())throw new IOException(Language.s(R.string.save_mkdir));
                    try(OutputStream file=new FileOutputStream(out)){
                        int n;while((n=zip.read(buffer))!=-1){
                            total+=n;if(total>MAX_BYTES)throw new IOException(Language.s(R.string.save_zip_size));
                            file.write(buffer,0,n);
                        }
                    }
                    zip.closeEntry();
                }
            }
            if(count==0)throw new IOException(Language.s(R.string.save_zip_empty));
            for(String name:names){
                File source=new File(stage,name.replace('/',File.separatorChar));
                File target=new File(root,name.replace('/',File.separatorChar));
                if(!target.getParentFile().isDirectory()&&!target.getParentFile().mkdirs())throw new IOException(Language.s(R.string.save_dest));
                Files.move(source.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING);
            }
        }finally{GameFiles.removeTree(stage);}
    }
}
