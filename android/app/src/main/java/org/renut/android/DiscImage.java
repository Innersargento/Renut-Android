package org.renut.android;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Bounded XDVDFS reader. No game data is bundled with the application. */
public final class DiscImage {
    public interface Progress { void update(long done, long total) throws IOException; }
    public static final class Entry {
        public final String path;
        public final long offset, size;
        Entry(String p, long o, long s) { path=p; offset=o; size=s; }
    }
    private final RandomAccessFile file;
    private final List<Entry> entries = new ArrayList<>();
    private final Set<Long> directories = new HashSet<>();
    private final Set<String> names = new HashSet<>();
    private long base, total, directoryBytes;
    public DiscImage(RandomAccessFile file) throws IOException {
        this.file=file;
        byte[] magic="MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
        boolean found=false;
        for (long candidate : new long[]{0, 0xFB20, 0x20600, 0x2080000, 0xFD90000}) {
            if (candidate+65536+2048 > file.length()) continue;
            file.seek(candidate+65536);
            byte[] header=new byte[2048]; file.readFully(header);
            if (Arrays.equals(magic, Arrays.copyOf(header,20)) &&
                Arrays.equals(magic, Arrays.copyOfRange(header,2028,2048))) {
                base=candidate; readDirectory(u32(header,20),u32(header,24),"",0); found=true; break;
            }
        }
        if (!found) throw new IOException(Language.s(R.string.disc_unknown));
        if (entries.stream().noneMatch(e -> e.path.equalsIgnoreCase("default.xex")))
            throw new IOException(Language.s(R.string.disc_no_default));
    }
    private static int u16(byte[] b,int p) { return (b[p]&255)|((b[p+1]&255)<<8); }
    private static long u32(byte[] b,int p) { return Integer.toUnsignedLong(u16(b,p)|(u16(b,p+2)<<16)); }
    private void range(long offset,long size) throws IOException {
        if (offset<0 || size<0 || offset>file.length() || size>file.length()-offset)
            throw new IOException(Language.s(R.string.disc_truncated));
    }
    private void readDirectory(long sector,long size,String parent,int depth) throws IOException {
        if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException(Language.s(R.string.import_cancelled));
        if (depth>64 || size<14 || size>32*1024*1024 || !directories.add(sector))
            throw new IOException(Language.s(R.string.disc_dir_cyclic));
        directoryBytes+=size;
        if(directoryBytes>64L*1024*1024) throw new IOException(Language.s(R.string.disc_meta_limit));
        long offset=base+sector*2048; range(offset,size);
        byte[] data=new byte[(int)size]; file.seek(offset); file.readFully(data);
        ArrayDeque<Integer> pending=new ArrayDeque<>(); pending.push(0);
        Set<Integer> seen=new HashSet<>();
        while (!pending.isEmpty()) {
            int p=pending.pop();
            if (!seen.add(p) || p<0 || p>data.length-14 || names.size()>100000)
                throw new IOException(Language.s(R.string.disc_tree_invalid));
            int length=data[p+13]&255;
            if (length==0 || p+14+length>data.length) throw new IOException(Language.s(R.string.disc_name_invalid));
            String name=new String(data,p+14,length,StandardCharsets.ISO_8859_1);
            if (name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\") || name.contains(":"))
                throw new IOException(Language.s(R.string.disc_path_invalid));
            for (char c:name.toCharArray()) if(c<32) throw new IOException(Language.s(R.string.disc_name_control));
            String path=parent+name;
            if (!names.add(path.toLowerCase(Locale.ROOT))) throw new IOException(Language.s(R.string.disc_name_dup));
            long childSector=u32(data,p+4), childSize=u32(data,p+8);
            if ((data[p+12]&16)!=0) {
                if(childSize>0) readDirectory(childSector,childSize,path+"/",depth+1);
            } else {
                long childOffset=base+childSector*2048; range(childOffset,childSize);
                total+=childSize;
                if(total>file.length()) throw new IOException(Language.s(R.string.disc_size_exceeds));
                entries.add(new Entry(path,childOffset,childSize));
            }
            int left=u16(data,p)*4,right=u16(data,p+2)*4;
            if(left!=0) pending.push(left);
            if(right!=0) pending.push(right);
        }
    }
    public List<Entry> entries() { return Collections.unmodifiableList(entries); }
    public long totalBytes() { return total; }
    public void extract(File destination,Progress progress) throws IOException {
        if (!destination.isDirectory()) throw new IOException(Language.s(R.string.disc_dest_missing));
        if (destination.getUsableSpace()<total+64L*1024*1024) throw new IOException(Language.s(R.string.disc_no_space));
        byte[] buffer=new byte[1024*1024]; long done=0;
        String root=destination.getCanonicalPath()+File.separator;
        for (Entry e:entries) {
            File out=new File(destination,e.path);
            if (!out.getCanonicalPath().startsWith(root)) throw new IOException(Language.s(R.string.disc_outside));
            if (!out.getParentFile().isDirectory() && !out.getParentFile().mkdirs()) throw new IOException(Language.s(R.string.disc_mkdir));
            if (!out.createNewFile()) throw new IOException(Language.s(R.string.disc_exists));
            file.seek(e.offset);
            try (OutputStream stream=new FileOutputStream(out)) {
                long remaining=e.size;
                while (remaining>0) {
                    if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException(Language.s(R.string.import_cancelled));
                    int amount=(int)Math.min(remaining,buffer.length);
                    file.readFully(buffer,0,amount); stream.write(buffer,0,amount);
                    remaining-=amount; done+=amount; progress.update(done,total);
                }
            }
        }
    }
}
