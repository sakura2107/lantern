package org.lantern.client;
import android.content.*;
import android.net.Uri;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.util.*;
import java.util.concurrent.*;
/** External-player APIs. All open/query methods run on a worker; no built-in movie player. */
final class MediaPlayback {
    static final Map<String,AutoCloseable> ACTIVE=new ConcurrentHashMap<>();
    static boolean active(){return !ACTIVE.isEmpty();}
    static void closeAll(){for(AutoCloseable session:new ArrayList<>(ACTIVE.values()))try{session.close();}catch(Exception ignored){}}
    static final class Track {
        final int index,kind;final String codec,language,title;
        Track(DataInputStream r)throws IOException{index=r.readInt();kind=r.readInt();codec=Remote.str(r);language=Remote.str(r);title=Remote.str(r);if(index<0||index>63||(kind!=1&&kind!=2))throw new IOException("Media track bound");}
    }
    static final class Tracks {
        final long size,durationMillis,bitrate;final String version;final int width,height;final boolean hdr;final List<Track> tracks;
        Tracks(DataInputStream r)throws IOException{
            if(r.readInt()!=1)throw new IOException("Media tracks version");size=r.readLong();version=Remote.str(r);durationMillis=r.readLong();bitrate=r.readLong();width=r.readInt();height=r.readInt();int flags=r.readInt();hdr=flags==1;
            int n=r.readInt();if(n<0||n>64||durationMillis<0||size<0||flags<0||flags>1)throw new IOException("Media tracks bounds");
            List<Track> out=new ArrayList<>();for(int i=0;i<n;i++)out.add(new Track(r));tracks=Collections.unmodifiableList(out);if(r.available()!=0)throw new IOException("Media tracks trailing bytes");
        }
    }
    static Tracks query(Remote.Credentials c,String path)throws Exception{
        try(Remote remote=new Remote(c,1,2)){
            String caps=Remote.str(Remote.reader(remote.call(30,new byte[0])));
            if(!caps.contains(";media-adapt=hevc-hls-fmp4-v2"))throw new IOException("电脑端未包含兼容的 fMP4 适配转码组件，请使用原画直读或下载后打开");
            return new Tracks(Remote.reader(remote.call(70,Remote.body(o->{o.writeInt(1);Remote.str(o,path);} ))));
        }
    }
    static final class Original implements AutoCloseable {
        final Context context;final Uri uri;final String mime,key;
        Original(Context context,Remote.Credentials c,String path)throws Exception{
            this.context=context.getApplicationContext();Remote.Info info;try(Remote remote=new Remote(c,1,2)){info=remote.stat(path,false);}
            uri=FilesProvider.share(new FilesProvider.Source(c,path,info));key=uri.getPathSegments().get(0);mime=FilesProvider.mime(path);
            ACTIVE.put(key,this);TransferPacer.SHARED.movie=true;try{TransferService.ensureMedia(this.context);}catch(RuntimeException error){close();throw error;}
        }
        public void close(){ACTIVE.remove(key);TransferPacer.SHARED.movie=active()||FilesProvider.activeProxies.get()>0;FilesProvider.revoke(uri);context.revokeUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION);}
    }
    static Original openOriginal(Context context,Remote.Credentials c,String path)throws Exception{return new Original(context,c,path);}
    static final class Adapted implements AutoCloseable {
        final Context context;final Remote.Credentials credentials;final String path,token;final Tracks tracks;
        final int width,height,audio,subtitle;final Uri uri;
        final ServerSocket server;final Thread acceptor;
        final ThreadPoolExecutor workers;
        final Set<Socket> sockets=ConcurrentHashMap.newKeySet();final Set<Remote> connections=ConcurrentHashMap.newKeySet();
        final Map<Integer,byte[]> cache=new LinkedHashMap<Integer,byte[]>(8,.75f,true){protected boolean removeEldestEntry(Map.Entry<Integer,byte[]> e){return size()>4;}};
        volatile boolean closed;volatile long lastRequest=System.nanoTime();volatile String failure="";
        Adapted(Context context,Remote.Credentials c,String path,int width,int height,int audio,int subtitle)throws Exception{
            if(width<320||width>2560||height<180||height>2160)throw new IOException("适配尺寸超出支持范围");
            this.context=context.getApplicationContext();credentials=c;this.path=path;this.width=width;this.height=height;this.audio=audio;this.subtitle=subtitle;
            tracks=query(c,path);if(tracks.hdr)throw new IOException("适配模式暂不支持 HDR 色调映射，请使用原画");
            if(tracks.durationMillis<=0||tracks.durationMillis>24L*60*60*1000)throw new IOException("适配模式需要不超过24小时的可定位媒体");
            byte[] random=new byte[16];new SecureRandom().nextBytes(random);token=Remote.hex(random);
            server=new ServerSocket();server.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"),0),8);server.setSoTimeout(1000);
            uri=Uri.parse("http://127.0.0.1:"+server.getLocalPort()+"/"+token+"/index.m3u8");
            workers=new ThreadPoolExecutor(2,2,0,TimeUnit.MILLISECONDS,new ArrayBlockingQueue<>(4),r->new Thread(r,"Lantern HLS request"));
            acceptor=new Thread(this::accept,"Lantern local HLS");
            ACTIVE.put(token,this);TransferPacer.SHARED.movie=true;try{TransferService.ensureMedia(this.context);}catch(RuntimeException error){close();throw error;}
            acceptor.start();
        }
        void accept(){while(!closed){
            try{Socket peer=server.accept();peer.setSoTimeout(10000);sockets.add(peer);
                try{workers.execute(()->serve(peer));}catch(RejectedExecutionException busy){sockets.remove(peer);peer.close();}
            }catch(SocketTimeoutException idle){if(System.nanoTime()-lastRequest>30L*60*1_000_000_000)close();}
            catch(IOException error){if(!closed)failure="本机播放通道已关闭";return;}
        }}
        byte[] playlist(){
            int count=(int)((tracks.durationMillis+1999)/2000);StringBuilder text=new StringBuilder("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MAP:URI=\"init.mp4\"\n");
            for(int i=0;i<count;i++)text.append(String.format(Locale.ROOT,"#EXTINF:%.3f,\nseg/%d.m4s\n",Math.min(2000,tracks.durationMillis-i*2000)/1000.0,i));
            text.append("#EXT-X-ENDLIST\n");return text.toString().getBytes(StandardCharsets.US_ASCII);
        }
        byte[] segment(int index)throws Exception{
            synchronized(cache){byte[] hit=cache.get(index);if(hit!=null)return hit;}
            long deadline=System.nanoTime()+30_000_000_000L;
            while(!closed){
                Set<Socket> attempts=ConcurrentHashMap.newKeySet();
                try(Remote remote=new Remote(credentials,1,2,s->{attempts.add(s);sockets.add(s);if(closed)s.close();})){
                    sockets.removeAll(attempts);
                    connections.add(remote);
                    try{
                        if(closed){remote.abort();throw new IOException("播放已结束");}
                        DataInputStream r=Remote.reader(remote.call(71,Remote.body(o->{o.writeInt(2);Remote.str(o,path);Remote.str(o,tracks.version);o.writeInt(width);o.writeInt(height);o.writeInt(audio);o.writeInt(subtitle);o.writeInt(index);} )));
                        if(r.readInt()!=2)throw new IOException("媒体分段版本");byte[] bytes=Remote.blob(r,Remote.MAX-512);if(r.available()!=0||bytes.length==0)throw new IOException("媒体分段无效");
                        synchronized(cache){cache.put(index,bytes);}return bytes;
                    }finally{connections.remove(remote);}
                }catch(Remote.RemoteError busy){if(busy.code!=8||System.nanoTime()>deadline){failure="分段 "+index+"："+busy.getMessage();throw busy;}Thread.sleep(100);}
                finally{sockets.removeAll(attempts);}
            }throw new IOException("播放已结束");
        }
        static int fragmentOffset(byte[] bytes)throws IOException{
            int at=0;boolean movie=false;
            while(at<=bytes.length-8){java.nio.ByteBuffer box=java.nio.ByteBuffer.wrap(bytes,at,bytes.length-at);long size=Integer.toUnsignedLong(box.getInt());int type=box.getInt(),header=8;
                if(size==1){if(box.remaining()<8)throw new IOException("MP4 box header");size=box.getLong();header=16;}
                if(size<header||size>bytes.length-at)throw new IOException("MP4 box bound");
                if(type==0x6d6f6f76)movie=true;
                if(type==0x6d6f6f66){if(!movie)throw new IOException("MP4 initialization missing");return at;}
                at+=(int)size;
            }throw new IOException("MP4 fragment missing");
        }
        static String line(InputStream input,int[] budget)throws IOException{
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();int previous=-1;
            while(budget[0]-->0){int c=input.read();if(c<0)throw new EOFException();if(previous==13&&c==10){byte[] data=bytes.toByteArray();return new String(data,0,data.length-1,StandardCharsets.US_ASCII);}bytes.write(c);previous=c;}
            throw new IOException("HTTP header bound");
        }
        void serve(Socket peer){try(Socket socket=peer){
            int[] budget={8192};InputStream input=socket.getInputStream();String[] request=line(input,budget).split(" ");
            if(request.length!=3||(!request[0].equals("GET")&&!request[0].equals("HEAD"))||!request[2].equals("HTTP/1.1")){reply(socket,400,"text/plain",new byte[0],false,0,0);return;}
            String host=null,range=null;for(int n=0;n<64;n++){String header=line(input,budget);if(header.isEmpty())break;int colon=header.indexOf(':');if(colon<1)throw new IOException("HTTP header");
                String name=header.substring(0,colon).trim().toLowerCase(Locale.ROOT),value=header.substring(colon+1).trim();if(name.equals("host"))host=value;if(name.equals("range"))range=value;}
            if(!("127.0.0.1:"+server.getLocalPort()).equals(host)||!request[1].startsWith("/"+token+"/")){reply(socket,404,"text/plain",new byte[0],false,0,0);return;}
            String file=request[1].substring(token.length()+2);byte[] bytes;String mime;
            if(file.equals("index.m3u8")){bytes=playlist();mime="application/vnd.apple.mpegurl";}
            else if(file.equals("init.mp4")){byte[] complete=segment(0);bytes=Arrays.copyOf(complete,fragmentOffset(complete));mime="video/mp4";}
            else if(file.matches("seg/[0-9]{1,5}\\.m4s")){
                int index=Integer.parseInt(file.substring(4,file.length()-4));if(index<0||index>=(tracks.durationMillis+1999)/2000){reply(socket,404,"text/plain",new byte[0],false,0,0);return;}
                byte[] complete=segment(index);bytes=Arrays.copyOfRange(complete,fragmentOffset(complete),complete.length);mime="video/iso.segment";
            }else{reply(socket,404,"text/plain",new byte[0],false,0,0);return;}
            lastRequest=System.nanoTime();int begin=0,end=bytes.length;boolean partial=false;
            if(range!=null){if(!range.matches("bytes=[0-9]+-[0-9]*")){reply(socket,416,mime,new byte[0],false,0,0);return;}
                String[] bounds=range.substring(6).split("-",-1);long start=Long.parseLong(bounds[0]),last=bounds[1].isEmpty()?bytes.length-1:Long.parseLong(bounds[1]);
                if(start>=bytes.length||last<start){reply(socket,416,mime,new byte[0],false,0,0);return;}begin=(int)start;end=(int)Math.min(bytes.length,last+1);partial=true;}
            reply(socket,partial?206:200,mime,bytes,request[0].equals("HEAD"),begin,end);
        }catch(Exception error){failure=error instanceof Remote.RemoteError?error.getMessage():"远程媒体读取失败："+error.getClass().getSimpleName();}
        finally{sockets.remove(peer);}}
        static void reply(Socket socket,int code,String type,byte[] bytes,boolean head,int begin,int end)throws IOException{
            String reason=code==200?"OK":code==206?"Partial Content":code==404?"Not Found":code==416?"Range Not Satisfiable":"Bad Request";
            String headers="HTTP/1.1 "+code+" "+reason+"\r\nContent-Type: "+type+"\r\nContent-Length: "+(end-begin)+"\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\nConnection: close\r\n";
            if(code==206)headers+="Content-Range: bytes "+begin+"-"+(end-1)+"/"+bytes.length+"\r\n";
            OutputStream output=socket.getOutputStream();output.write((headers+"\r\n").getBytes(StandardCharsets.US_ASCII));if(!head)output.write(bytes,begin,end-begin);output.flush();
        }
        public void close(){if(closed)return;closed=true;ACTIVE.remove(token);TransferPacer.SHARED.movie=active()||FilesProvider.activeProxies.get()>0;try{server.close();}catch(IOException ignored){}
            workers.shutdownNow();for(Socket socket:sockets)try{socket.close();}catch(IOException ignored){}for(Remote remote:connections)try{remote.abort();}catch(IOException ignored){}
            synchronized(cache){cache.clear();}}
    }
    static Adapted openAdapted(Context context,Remote.Credentials c,String path,int width,int height,int audio,int subtitle)throws Exception{return new Adapted(context,c,path,width,height,audio,subtitle);}
}
