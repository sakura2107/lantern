package org.lantern.client;
import android.view.*;
import java.io.*;
import java.security.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.*;
/** Only Lantern's own view and generated host files; no screenshots or external app interaction. */
final class V03Validation {
    interface Condition{boolean ready();}
    static void until(Condition c,long ms,String failure)throws Exception{long end=System.nanoTime()+ms*1_000_000;while(!c.ready()&&System.nanoTime()<end)Thread.sleep(25);if(!c.ready())throw new IOException(failure);}
    static void run(DeviceTestRunner t,MainActivity a,Remote.Credentials provisioned)throws Exception {
        Remote.Credentials c=a.credentials;
        a.video.preferHevc=!t.v03AvcOnly;
        JSONArray caps=new JSONArray();for(DecoderCapabilities.Limit l:DecoderCapabilities.query())caps.put(new JSONObject().put("mime",l.mime).put("width",l.width).put("height",l.height).put("fps",l.fps));
        t.report.put("decoder_capability_queries",caps);
        List<DesktopSession.Display> displays=DesktopSession.limits(c);t.check(!displays.isEmpty(),"native monitor/capability query");
        DesktopSession.Display display=displays.get(0);
        try(Remote remote=new Remote(c,3,3)){
            File marker=new File(a.getCacheDir(),"v03-generated-marker");
            try(FileOutputStream out=new FileOutputStream(marker)){out.write(new byte[]{1,2,3});}
            try{remote.upload(marker,"tone-start.bin",new AtomicBoolean(),(n,total)->{});}finally{marker.delete();}
        }
        a.systemAudio.measureSamples=true;a.systemAudio.start(c);
        t.runOnMainSync(()->{a.hardwareDesktop=true;a.subscribed.set(true);a.display.set(true);a.ui.select(AppShell.DESKTOP);a.updateDesktopPresentation();});
        JSONArray matrix=new JSONArray();String streamId=null;long streamEpoch=-1;
        try {
            for(boolean automatic:new boolean[]{false,true})for(int width:automatic?new int[]{3840}:new int[]{1280,1920,2560,3840})for(int fps:new int[]{30,60,120}){
                AvcDesktop.Profile profile=new AvcDesktop.Profile(width,fps,12_000_000,0,automatic);
                t.runOnMainSync(()->{a.videoProfile=profile;a.video.start(c,profile);});
                until(()->a.video.activeSession!=null&&a.video.width>0&&a.video.failure.isEmpty(),20000,"desktop startup: "+a.video.failure);
                // Control is asynchronous. Wait for the acknowledged dimensions/rate metadata, then sample.
                DecoderCapabilities.Limit decoder=DecoderCapabilities.best(!t.v03AvcOnly&&display.hevc,display.width,display.height,fps);
                int expectedFps=Math.min(fps,Math.min(display.refresh,decoder.fps));
                int expectedWidth=Math.min(width,Math.min(display.width,decoder.width));
                if(!automatic)until(()->a.video.activeSession!=null&&a.video.activeSession.status.fps==expectedFps&&a.video.activeSession.status.width==expectedWidth,
                    10000,"online quality metadata not applied: "+width+"/"+fps+" "+a.video.failure);
                Thread.sleep(400);
                DesktopSession current=a.video.activeSession;
                if(streamId==null){streamId=current.id;streamEpoch=a.video.streamEpoch.get();}
                t.check(current.id.equals(streamId)&&a.video.streamEpoch.get()==streamEpoch,"online switch preserves TLS stream");
                long began=System.nanoTime(),before=a.video.received.get(),bytes=a.video.encodedBytes.get();long updates=a.avcView.updateCount();
                Thread.sleep(1200);double seconds=(System.nanoTime()-began)/1e9;
                long decoded=a.video.received.get()-before,shown=a.avcView.updateCount()-updates;
                DesktopSession.Status status=current.status;
                if(t.v03AvcOnly)t.check(status.codec.equals("h264"),"H264 hardware fallback decoded");
                t.check(decoded>0&&shown>0,"actual MediaCodec and TextureView updates for preset "+width+"/"+fps);
                matrix.put(new JSONObject().put("requested_width",width).put("requested_fps",fps).put("automatic",automatic)
                    .put("actual_width",status.width).put("actual_height",status.height).put("configured_fps",status.fps)
                    .put("codec",status.codec).put("path",status.path).put("window_s",seconds).put("received_fps",decoded/seconds)
                    .put("texture_update_fps",shown/seconds).put("payload_mbps",(a.video.encodedBytes.get()-bytes)*8/seconds/1e6));
            }
            t.report.put("quality_matrix",matrix);
            t.check(a.systemAudio.failure.isEmpty()&&a.systemAudio.packets.get()>0,"independent system audio remains active during all switches");
            t.report.put("audio_path",a.systemAudio.path).put("audio_wire_bytes",a.systemAudio.wireBytes.get()).put("audio_pcm_bytes",a.systemAudio.pcmBytes.get())
                .put("audio_underruns",a.systemAudio.underruns).put("audio_tone_samples_nonzero",a.systemAudio.nonZeroSamples.get());
            t.fullscreenChecks(a);
            try(InputChannel input=new InputChannel(c,null)){
                for(int i=0;i<64;i++)t.check(input.submit(40,new byte[0]),"reliable focus command admission");
                until(()->input.acknowledged()==64,15000,"Java reliable input did not acknowledge all commands");
                t.check(input.pending()==0,"all reliable input acknowledged");
                until(()->input.acknowledged()==65,12000,"idle 10 second reliable heartbeat missing");
                t.check(input.state().phase==InputChannel.Phase.NORMAL,"idle input remains normal");
            }
            long began=System.nanoTime(),position=0;MessageDigest digest=MessageDigest.getInstance("SHA-256");
            RemoteFileReader ownedReader=new RemoteFileReader(c,"large.bin");
            try(RemoteFileReader reader=ownedReader){
                byte[] buffer=new byte[262144];while(position<reader.getSize()){int n=reader.readAt(position,buffer,0,buffer.length);if(n<=0)throw new IOException("Original read ended early");digest.update(buffer,0,n);position+=n;}
                String expected;try(Remote remote=new Remote(c,1,2)){expected=remote.stat("large.bin",true).hash;}
                t.check(Remote.hex(digest.digest()).equals(expected),"read-ahead original movie/file bytes remain SHA verified");
                byte[] seek=new byte[4096];long seekStart=System.nanoTime();reader.readAt(0,seek,0,seek.length);
                t.report.put("original_seek_ms",(System.nanoTime()-seekStart)/1e6);
                t.report.put("original_cache_blocks",reader.cache.size());t.check(reader.cache.size()<=RemoteFileReader.KEEP,"bounded read-ahead cache");
                synchronized(reader.lock){t.check(reader.extra.size()<=4,"live read connections remain bounded after seek/reconnect");}
            }
            synchronized(ownedReader.lock){t.check(ownedReader.extra.isEmpty(),"close releases every tracked original reader connection");}
            t.report.put("original_read_bytes",position).put("original_read_including_sha_seconds",(System.nanoTime()-began)/1e9);
            t.report.put("optical_latency_measured",false).put("4K_source_is_native_monitor",false);
        }catch(Exception error){
            t.report.put("video_failure",a.video.failure).put("hardware_desktop",a.hardwareDesktop)
                .put("video_width",a.video.width).put("video_received",a.video.received.get())
                .put("video_desired",a.video.desired!=null).put("video_surface",a.avcView.current!=null);
            t.runOnMainSync(()->{try{t.report.put("ui_notice",a.ui.status.getText().toString()).put("selected_page",a.ui.selected)
                .put("video_visible",a.avcView.isShown()).put("pane_width",a.desktopLayout.pane.getWidth()).put("pane_height",a.desktopLayout.pane.getHeight());}catch(Exception ignored){}});
            throw error;
        }finally{
            t.runOnMainSync(()->{a.subscribed.set(false);a.video.stop();a.systemAudio.stop();a.updateDesktopPresentation();});
        }
    }
}
