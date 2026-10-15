package org.lantern.client;
import android.content.ContentResolver;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.*;
import java.nio.channels.FileChannel;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;
/** One retained provider descriptor: hash, seek and upload without a full local snapshot. */
final class SafSource implements Remote.UploadSource {
    final ContentResolver resolver;final Uri uri;final ParcelFileDescriptor descriptor;
    final FileInputStream input;final FileChannel channel;final long[] version;private String digest;
    private SafSource(ContentResolver r,Uri u,ParcelFileDescriptor p,long[] v)throws IOException{resolver=r;uri=u;descriptor=p;version=v;input=new ParcelFileDescriptor.AutoCloseInputStream(p);channel=input.getChannel();}
    // Null means the provider cannot offer known-size seekable access; caller retains snapshot fallback.
    static SafSource open(ContentResolver resolver,Uri uri)throws IOException{
        long[] version=TreeUpload.version(resolver,uri);if(version[0]<0)return null;
        ParcelFileDescriptor descriptor=resolver.openFileDescriptor(uri,"r");if(descriptor==null)throw new IOException("源应用未提供文件描述符");SafSource source=new SafSource(resolver,uri,descriptor,version);
        try{if(source.channel.size()!=version[0]){source.close();return null;}source.channel.position(version[0]);source.channel.position(0);return source;}
        catch(IOException notSeekable){source.close();return null;}
    }
    public long size(){return version[0];}
    public String hash(AtomicBoolean cancel)throws Exception{if(digest==null)digest=Remote.sourceHash(this,cancel);validate();return digest;}
    public void seek(long offset)throws IOException{if(offset<0||offset>size())throw new IOException("源偏移越界");channel.position(offset);}
    public int read(byte[] buffer,int limit)throws IOException{return input.read(buffer,0,limit);}
    public void validate()throws IOException{if(channel.size()!=size()||!Arrays.equals(version,TreeUpload.version(resolver,uri)))throw new IOException("源文件在读取期间变化，远端原件已保留");}
    public void close()throws IOException{input.close();}
}
