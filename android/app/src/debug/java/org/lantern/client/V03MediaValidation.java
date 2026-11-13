package org.lantern.client;
import android.media.*;
import android.os.*;
import android.view.*;
import android.widget.FrameLayout;
import java.io.*;
import java.net.*;
import java.util.concurrent.*;
/** Debug-only generated-movie validation. No movie player is added to production UI. */
final class V03MediaValidation {
    static void run(DeviceTestRunner t,MainActivity activity,Remote.Credentials c)throws Exception{
        MediaPlayback.Tracks tracks=MediaPlayback.query(c,"movie.mkv");t.check(tracks.width==3840&&tracks.height==2160,"physical phone track query: original 4K source");
        t.check(tracks.tracks.size()==2&&tracks.tracks.get(1).codec.equals("ass"),"phone audio and ASS tracks");
        AvcView[] view={null};AvcView.Target[] surface={null};
        t.runOnMainSync(()->{view[0]=new AvcView(activity);activity.setContentView(view[0],new FrameLayout.LayoutParams(-1,-1));});
        V03Validation.until(()->view[0].current!=null,5000,"debug movie Surface unavailable");
        t.runOnMainSync(()->surface[0]=view[0].acquire());
        MediaPlayer player=new MediaPlayer();CountDownLatch prepared=new CountDownLatch(1),rendered=new CountDownLatch(1),seeked=new CountDownLatch(1),completed=new CountDownLatch(1);
        String[] error={null};long began=System.nanoTime();
        try(MediaPlayback.Adapted adapted=MediaPlayback.openAdapted(activity,c,"movie.mkv",1280,720,1,2)){
            t.check(adapted.server.getInetAddress().isLoopbackAddress(),"HLS listener is loopback only");
            URL url=new URL(adapted.uri.toString());HttpURLConnection denied=(HttpURLConnection)new URL(adapted.uri.toString().replace(adapted.token,"wrong-token")).openConnection();
            try{t.check(denied.getResponseCode()==404,"local HLS token required");}finally{denied.disconnect();}
            HttpURLConnection playlist=(HttpURLConnection)url.openConnection();String text;
            try{t.check(playlist.getResponseCode()==200,"local HLS playlist");text=new String(playlist.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.US_ASCII);}finally{playlist.disconnect();}
            t.check(text.contains("#EXT-X-ENDLIST")&&text.contains("seg/1.ts")&&text.contains("#EXT-X-VERSION:3")&&!text.contains("#EXT-X-MAP"),"seekable MPEG-TS VOD segment list");
            player.setSurface(surface[0].surface);player.setOnPreparedListener(p->prepared.countDown());player.setOnSeekCompleteListener(p->seeked.countDown());
            player.setOnInfoListener((p,what,extra)->{if(what==MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START)rendered.countDown();return false;});
            player.setOnVideoSizeChangedListener((p,w,h)->{try{t.report.put("hls_video_width",w).put("hls_video_height",h);}catch(Exception ignored){}});
            player.setOnCompletionListener(p->completed.countDown());
            player.setOnErrorListener((p,what,extra)->{error[0]="MediaPlayer "+what+"/"+extra;prepared.countDown();rendered.countDown();seeked.countDown();return true;});
            player.setDataSource(activity,adapted.uri);player.prepareAsync();
            t.check(prepared.await(40,TimeUnit.SECONDS)&&error[0]==null,"Android HLS prepare: "+error[0]+" "+adapted.failure);
            player.start();boolean firstFrame=rendered.await(15,TimeUnit.SECONDS);
            t.report.put("hls_surface_updates",view[0].updateCount()).put("hls_video_width",player.getVideoWidth()).put("hls_video_height",player.getVideoHeight()).put("hls_position_ms",player.getCurrentPosition());
            org.json.JSONArray selectedTracks=new org.json.JSONArray();for(MediaPlayer.TrackInfo track:player.getTrackInfo())selectedTracks.put(track.toString());t.report.put("hls_player_tracks",selectedTracks);
            t.check(firstFrame&&error[0]==null,"Android system MediaPlayer HLS first displayed frame: "+error[0]+" "+adapted.failure);
            t.report.put("hls_first_frame_seconds",(System.nanoTime()-began)/1e9);
            t.check(player.getVideoWidth()==1280&&player.getVideoHeight()==720,"phone receives client-sized H.264");
            Thread.sleep(300);t.check(view[0].updateCount()>0,"actual phone movie surface updates");
            long seekStart=System.nanoTime();player.seekTo(2500);
            t.check(seeked.await(15,TimeUnit.SECONDS)&&error[0]==null,"HLS random seek completed");
            t.report.put("hls_seek_callback_ms",(System.nanoTime()-seekStart)/1e6).put("hls_position_after_seek_ms",player.getCurrentPosition());
            long afterSeek=view[0].updateCount();t.check(completed.await(35,TimeUnit.SECONDS)&&error[0]==null,"HLS plays through final audio-padded segment");
            t.check(view[0].updateCount()>afterSeek,"HLS continues displaying frames after seek");
            t.report.put("hls_completed_position_ms",player.getCurrentPosition()).put("hls_total_surface_updates",view[0].updateCount());
            HttpURLConnection range=(HttpURLConnection)new URL(adapted.uri.toString().replace("index.m3u8","seg/1.ts")).openConnection();range.setRequestProperty("Range","bytes=100-200");
            try{t.check(range.getResponseCode()==206&&range.getInputStream().readAllBytes().length==101,"HLS segment HTTP range");}finally{range.disconnect();}
            t.report.put("hls_cache_segments",adapted.cache.size()).put("platform_mediaplayer_hls_verified",true).put("external_player_app_operated",false);
        }finally{try{player.release();}finally{if(surface[0]!=null)surface[0].release();}}
        try(MediaPlayback.Original original=MediaPlayback.openOriginal(activity,c,"movie.mkv")){
            try(ParcelFileDescriptor fd=activity.getContentResolver().openFileDescriptor(original.uri,"r")){
                t.check(fd!=null&&fd.getStatSize()>0,"original player URI remains seekable");
                try(InputStream bytes=new ParcelFileDescriptor.AutoCloseInputStream(fd)){t.check(bytes.read(new byte[4096])>0,"original player URI remote bytes");}
            }
        }
        t.check(!MediaPlayback.active(),"movie sessions and background service lease released");
        t.report.put("builtin_movie_ui_added",false).put("hls_ass_track_selected",2).put("hls_output_audio","AAC stereo");
    }
}
