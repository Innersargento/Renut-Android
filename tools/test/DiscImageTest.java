import org.renut.android.DiscImage;
import org.renut.android.GameFiles;
import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;

public final class DiscImageTest {
    private static void u32(byte[] b,int p,int n){for(int i=0;i<4;i++)b[p+i]=(byte)(n>>>(i*8));}
    private static byte[] fixture(String name){
        byte[] b=new byte[35*2048];byte[] magic="MICROSOFT*XBOX*MEDIA".getBytes();
        System.arraycopy(magic,0,b,65536,20);System.arraycopy(magic,0,b,65536+2028,20);
        u32(b,65536+20,33);u32(b,65536+24,2048);
        int p=33*2048;u32(b,p+4,34);u32(b,p+8,24);b[p+13]=(byte)name.length();
        System.arraycopy(name.getBytes(),0,b,p+14,name.length());
        System.arraycopy("XEX2".getBytes(),0,b,34*2048,4);return b;
    }
    private static void rejected(Path path,byte[] bytes)throws Exception{
        Files.write(path,bytes);
        try(RandomAccessFile f=new RandomAccessFile(path.toFile(),"r")){
            try{new DiscImage(f);throw new AssertionError("Malformed disc accepted");}catch(IOException expected){}
        }
    }
    public static void main(String[] args)throws Exception{
        Path work=Files.createTempDirectory(Path.of("out"),"disc-test-");
        try{
            Path iso=work.resolve("fixture.iso");Files.write(iso,fixture("default.xex"));
            try(RandomAccessFile f=new RandomAccessFile(iso.toFile(),"r")){
                DiscImage disc=new DiscImage(f);
                if(disc.entries().size()!=1||disc.totalBytes()!=24)throw new AssertionError();
                File destination=Files.createDirectory(work.resolve("extract")).toFile();
                disc.extract(destination,(done,total)->{});
                if(!GameFiles.identify(new File(destination,"default.xex")).matches("[0-9a-f]{64}"))throw new AssertionError();
            }
            rejected(iso,fixture("../bad.xex"));
            byte[] truncated=fixture("default.xex");u32(truncated,33*2048+4,Integer.MAX_VALUE);rejected(iso,truncated);
            byte[] cyclic=fixture("default.xex");cyclic[33*2048]=1;rejected(iso,cyclic);
            rejected(iso,new byte[20]);
            System.out.println("PASS: valid extraction, traversal rejection, out-of-range rejection, malformed tree rejection, short image rejection.");
            if(args.length>0)try(RandomAccessFile real=new RandomAccessFile(args[0],"r")){
                DiscImage disc=new DiscImage(real);
                DiscImage.Entry xex=disc.entries().stream().filter(e->e.path.equalsIgnoreCase("default.xex")).findFirst().orElseThrow();
                MessageDigest hash=MessageDigest.getInstance("SHA-256");byte[] buffer=new byte[1024*1024];
                real.seek(xex.offset);long remaining=xex.size;
                while(remaining>0){int n=(int)Math.min(remaining,buffer.length);real.readFully(buffer,0,n);hash.update(buffer,0,n);remaining-=n;}
                String value=HexFormat.of().formatHex(hash.digest());
                if(!value.equals(GameFiles.EXPECTED))throw new AssertionError("European XEX mismatch");
                System.out.println("PASS real ISO: files="+disc.entries().size()+", extractedBytes="+disc.totalBytes()+", XEX="+value);
            }
        }finally{GameFiles.removeTree(work.toFile());}
    }
}
