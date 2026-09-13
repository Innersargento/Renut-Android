package org.renut.android;

import java.io.*;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.*;
import java.util.Locale;

public final class GameFiles {
    public static final String EXPECTED="d57b15b73490ca8334fb2fc7a53e0b21936a187731fa4f891273f4feaf38d3bc";
    public static String identify(File file) throws IOException {
        if(file.length()<24 || file.length()>256L*1024*1024) throw new IOException("Tamanho XEX inválido.");
        try(InputStream in=new FileInputStream(file)) {
            byte[] magic=new byte[4];
            if(in.read(magic)!=4 || magic[0]!='X' || magic[1]!='E' || magic[2]!='X' || magic[3]!='2')
                throw new IOException("O executável não tem cabeçalho XEX2.");
        }
        try(InputStream in=new FileInputStream(file)) {
            MessageDigest digest=MessageDigest.getInstance("SHA-256"); byte[] b=new byte[1024*1024]; int n;
            while((n=in.read(b))!=-1) digest.update(b,0,n);
            StringBuilder out=new StringBuilder();
            for(byte x:digest.digest()) out.append(String.format(Locale.ROOT,"%02x",x&255));
            return out.toString();
        } catch(NoSuchAlgorithmException e) { throw new IOException(e); }
    }
    public static void requireEuropean(File file) throws IOException {
        if(!EXPECTED.equals(identify(file))) throw new IOException("XEX diferente da edição europeia cadastrada. O jogo anterior foi preservado.");
    }
    public static void atomicWrite(File destination,String text) throws IOException {
        File temp=new File(destination.getParentFile(),destination.getName()+".tmp");
        Files.write(temp.toPath(),text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        Files.move(temp.toPath(),destination.toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);
    }
    public static void removeTree(File root) throws IOException {
        if(!root.exists()) return;
        if(!Files.isSymbolicLink(root.toPath()) && root.isDirectory()) {
            File[] files=root.listFiles();
            if(files==null) throw new IOException("Não foi possível acessar a pasta temporária.");
            for(File child:files) removeTree(child);
        }
        if(!root.delete()) throw new IOException("Não foi possível remover arquivo temporário.");
    }
}
