package org.lantern.client;
import android.media.*;
import android.os.*;
import java.io.*;
import java.net.*;
import java.nio.*;
import java.util.*;
/** Debug HLS consumer: HTTP playlist/TS segments -> platform extractor/codec -> Lantern Surface.
 * Android MediaPlayer's HLS demuxer is tested separately; no third-party player is operated.
 */
final class V03HlsDecoderValidation {
    static final class BytesSource extends MediaDataSource {
        final byte[] bytes;BytesSource(byte[] b){bytes=b;}
        public int readAt(long position,byte[] output,int offset,int size){if(position>=bytes.length)return -1;if(position<0)return -1;int n=Math.min(size,bytes.length-(int)position);System.arraycopy(bytes,(int)position,output,offset,n);return n;}
        public long getSize(){return bytes.length;}public void close(){}
    }
    static byte[] get(URL url,String method,String range,int expected)throws Exception{
        HttpURLConnection connection=(HttpURLConnection)url.openConnection();connection.setConnectTimeout(10000);connection.setReadTimeout(30000);connection.setRequestMethod(method);
        if(range!=null)connection.setRequestProperty("Range",range);
        try{if(connection.getResponseCode()!=expected)throw new IOException("HLS HTTP "+connection.getResponseCode());
            if(method.equals("HEAD")||expected>=400)return new byte[0];return connection.getInputStream().readAllBytes();}finally{connection.disconnect();}
    }
    static final class Decoded {long frames,pcmBytes,firstPts=-1;int width,height;}
    static long firstVideoTime(byte[] bytes)throws Exception{MediaExtractor first=new MediaExtractor();try{first.setDataSource(new BytesSource(bytes));for(int i=0;i<first.getTrackCount();i++)if(first.getTrackFormat(i).getString(MediaFormat.KEY_MIME).startsWith("video/")){first.selectTrack(i);return first.getSampleTime();}throw new IOException("Video timeline missing");}finally{first.release();}}
    static final class Fetched {final byte[] bytes;final double seconds;Fetched(byte[] b,double s){bytes=b;seconds=s;}}
    static Fetched fetch(URL url)throws Exception{long start=System.nanoTime();return new Fetched(get(url,"GET",null,200),(System.nanoTime()-start)/1e9);}
    static boolean hasVideo(byte[] bytes)throws Exception{MediaExtractor e=new MediaExtractor();try{e.setDataSource(new BytesSource(bytes));for(int i=0;i<e.getTrackCount();i++){String m=e.getTrackFormat(i).getString(MediaFormat.KEY_MIME);if(m!=null&&m.startsWith("video/"))return true;}return false;}finally{e.release();}}
    static Decoded decode(byte[] bytes,android.view.Surface surface,boolean paced,long start,long origin)throws Exception{return decode(bytes,surface,paced,start,origin,true);}
    static Decoded decode(byte[] bytes,android.view.Surface surface,boolean paced,long start,long origin,boolean withVideo)throws Exception{
        Decoded result=new Decoded();
        for(boolean video:withVideo?new boolean[]{true,false}:new boolean[]{false}){
            MediaExtractor extractor=new MediaExtractor();MediaCodec codec=null;
            try{extractor.setDataSource(new BytesSource(bytes));MediaFormat format=null;
                for(int i=0;i<extractor.getTrackCount();i++){MediaFormat candidate=extractor.getTrackFormat(i);String mime=candidate.getString(MediaFormat.KEY_MIME);
                    if(mime!=null&&mime.startsWith(video?"video/":"audio/")){format=candidate;extractor.selectTrack(i);break;}}
                if(format==null)throw new IOException("segment missing "+(video?"video":"audio")+" track");
                String mime=format.getString(MediaFormat.KEY_MIME);
                if(video&&!mime.equals("video/avc")||!video&&!mime.equals("audio/mp4a-latm"))throw new IOException("Unexpected adapted codec "+mime);
                if(video){result.width=format.getInteger(MediaFormat.KEY_WIDTH);result.height=format.getInteger(MediaFormat.KEY_HEIGHT);}
                codec=MediaCodec.createDecoderByType(mime);codec.configure(format,video?surface:null,null,0);codec.start();
                boolean inputEnd=false,outputEnd=false;MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();long deadline=System.nanoTime()+15_000_000_000L;
                while(!outputEnd){if(System.nanoTime()>deadline)throw new IOException("Platform codec drain timeout");
                    if(!inputEnd){int index=codec.dequeueInputBuffer(10000);if(index>=0){ByteBuffer buffer=codec.getInputBuffer(index);int n=extractor.readSampleData(buffer,0);
                        if(n<0){codec.queueInputBuffer(index,0,0,0,MediaCodec.BUFFER_FLAG_END_OF_STREAM);inputEnd=true;}
                        else{codec.queueInputBuffer(index,0,n,extractor.getSampleTime(),0);extractor.advance();}}}
                    int index=codec.dequeueOutputBuffer(info,10000);
                    if(index>=0){boolean data=info.size>0;
                        if(data&&video){if(result.firstPts<0)result.firstPts=info.presentationTimeUs;
                            if(paced){long due=start+(info.presentationTimeUs-origin)*1000;long remaining=due-System.nanoTime();if(remaining>0)Thread.sleep(remaining/1_000_000,(int)(remaining%1_000_000));}
                            ++result.frames;
                        }else if(data)result.pcmBytes+=info.size;
                        outputEnd=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;codec.releaseOutputBuffer(index,video&&data);
                    }
                }
            }finally{if(codec!=null)codec.release();extractor.release();}
        }return result;
    }
    static void run(DeviceTestRunner t,MainActivity activity,Remote.Credentials c)throws Exception{
        MediaPlayback.Tracks tracks=MediaPlayback.query(c,"movie.mkv");
        t.check(tracks.width==3840&&tracks.height==2160,"generated 4K source tracks");
        t.check(tracks.tracks.size()==2&&tracks.tracks.get(1).codec.equals("ass"),"generated audio/ASS tracks");
        TextureSink[] view={null};TextureSink.Target[] target={null};t.runOnMainSync(()->{view[0]=new TextureSink(activity);activity.setContentView(view[0]);});
        V03Validation.until(()->view[0].current!=null,5000,"Lantern decode Surface unavailable");t.runOnMainSync(()->target[0]=view[0].acquire());
        long began=System.nanoTime();
        java.util.concurrent.ExecutorService ahead=java.util.concurrent.Executors.newSingleThreadExecutor();
        try(MediaPlayback.Adapted adapted=MediaPlayback.openAdapted(activity,c,"movie.mkv",1280,720,1,2)){
            URL playlist=new URL(adapted.uri.toString());t.check(adapted.server.getInetAddress().isLoopbackAddress(),"HLS loopback binding");
            get(new URL(playlist.toString().replace(adapted.token,"invalid-token")),"GET",null,404);t.check(true,"HLS requires random token");
            String text=new String(get(playlist,"GET",null,200),java.nio.charset.StandardCharsets.US_ASCII);
            t.check(text.contains("#EXT-X-VERSION:3")&&!text.contains("#EXT-X-MAP")&&text.contains("#EXT-X-ENDLIST"),"MPEG-TS HLS VOD playlist");
            get(new URL(playlist,"seg/0.ts"),"HEAD",null,200);t.check(true,"HLS HEAD supported");
            org.json.JSONArray segments=new org.json.JSONArray();long frames=0,pcm=0,playStart=0,firstUpdates=view[0].updateCount();
            List<String> paths=new ArrayList<>();for(String line:text.split("\n"))if(line.startsWith("seg/"))paths.add(line.trim());
            java.util.concurrent.Future<Fetched> pending=ahead.submit(()->fetch(new URL(playlist,paths.get(0))));
            int count=0;
            for(String line:paths){
                t.report.put("hls_current_segment",count).put("hls_segments",segments);
                Fetched fetched=pending.get();byte[] segment=fetched.bytes;
                if(count+1<paths.size()){final String next=paths.get(count+1);pending=ahead.submit(()->fetch(new URL(playlist,next)));}
                t.check(segment.length>=188&&segment.length%188==0&&segment[0]==0x47,"segment "+count+" is MPEG-TS");
                if(count==0)playStart=System.nanoTime();
                // A lone TS file never releases its final H.264 access unit to MediaExtractor (no following start
                // code), so a full segment yields one picture less and a one-picture tail shows no video track.
                // Players read the segments as one stream, where the next segment completes it.
                boolean last=count==paths.size()-1,video=hasVideo(segment);long origin=video?firstVideoTime(segment):0;
                if(!video)t.check(last,"only the one-picture tail segment may lack an extractable picture");
                Decoded decoded=decode(segment,target[0].surface,true,playStart+count*2_000_000_000L,origin,video);frames+=decoded.frames;pcm+=decoded.pcmBytes;
                t.check(decoded.pcmBytes>0&&(!video||decoded.width==1280&&decoded.height==720&&(decoded.frames>=58||last&&decoded.frames>0)),"phone independently decodes H.264/AAC segment "+count);
                segments.put(new org.json.JSONObject().put("segment",count).put("fetch_seconds",fetched.seconds).put("bytes",segment.length).put("video_frames",decoded.frames).put("pcm_bytes",decoded.pcmBytes).put("first_pts_us",origin));++count;
            }
            V03Validation.until(()->view[0].updateCount()>firstUpdates,3000,"No actual phone Surface updates");
            t.check(count==MediaPlayback.Adapted.segmentCount(tracks.durationMillis),"all HLS segments including the tail consumed");
            // 48 kHz stereo 16-bit: the segments together carry the movie's audio once, without per-segment repeats.
            double audioSeconds=pcm/(48000.0*4);t.report.put("hls_audio_seconds",audioSeconds);
            t.check(Math.abs(audioSeconds-tracks.durationMillis/1000.0)<0.1,"segment audio tiles the timeline without overlap: "+audioSeconds);
            long seek=System.nanoTime();byte[] sought=get(new URL(playlist,"seg/2.ts"),"GET",null,200);Decoded soughtDecoded=decode(sought,target[0].surface,false,0,0);
            t.check(soughtDecoded.frames>0,"random HLS seek independently decodes");
            byte[] range=get(new URL(playlist,"seg/1.ts"),"GET","bytes=100-200",206);t.check(range.length==101,"HLS HTTP byte range");
            t.report.put("hls_validation_client","debug HTTP HLS + Android MediaExtractor/MediaCodec").put("hls_segments",segments)
                .put("hls_seek_decode_ms",(System.nanoTime()-seek)/1e6).put("hls_total_video_frames",frames).put("hls_decoded_aac_pcm_bytes",pcm)
                .put("hls_total_surface_updates",view[0].updateCount()-firstUpdates).put("hls_total_seconds",(System.nanoTime()-began)/1e9)
                .put("hls_first_surface_seconds",(view[0].updateStamps()[0]-began)/1e9).put("external_player_app_operated",false);
        }finally{ahead.shutdownNow();if(target[0]!=null)target[0].release();}
        t.check(!MediaPlayback.active(),"HLS session lease closed");
    }
}
