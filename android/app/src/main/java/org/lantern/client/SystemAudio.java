package org.lantern.client;

import android.content.Context;
import android.media.*;
import android.os.*;
import java.io.*;
import java.util.concurrent.atomic.AtomicLong;

/** Independent, bounded PCM loopback receiver. Owns network and AudioTrack on one worker. */
final class SystemAudio {
    interface Events { void started(int rate); void failed(String reason); void reconnecting(String reason); }
    java.util.function.Supplier<MediaTimeline> timelineSource=MediaTimeline::new;
    final Context context; final Events events;
    final Handler main=new Handler(Looper.getMainLooper());
    final Thread worker;
    static final class Request { final Remote.Credentials credentials; MediaTimeline timeline;int attempts;long retryAt;Request(Remote.Credentials c){credentials=c;} }
    volatile Request desired;
    volatile Request readyRequest;
    volatile Request failedRequest;
    volatile Remote active;
    volatile java.net.Socket opening;
    volatile boolean closed;
    volatile String failure="";
    volatile String encoding="PCM16LE";
    final AtomicLong wireBytes=new AtomicLong();
    final AtomicLong reconnects=new AtomicLong();
    volatile boolean measureSamples; // Explicit debug validation only; production does not scan sample contents.
    volatile int sampleRate, underruns, performanceMode;
    volatile long playedFrames, writtenFrames, firstPacketNs;
    volatile double tonePowerSum;
    volatile long analyzedPackets;
    final AtomicLong packets=new AtomicLong(), pcmBytes=new AtomicLong(), nonZeroSamples=new AtomicLong();
    final AtomicLong squaredSamples=new AtomicLong(), sampleCount=new AtomicLong();
    SystemAudio(Context c, Events e) { context=c; events=e; worker=new Thread(this::loop,"Lantern system audio"); worker.start(); }
    synchronized void start(Remote.Credentials c) { if(closed || (desired!=null && desired.credentials==c)) return; desired=new Request(c);desired.timeline=timelineSource.get();failedRequest=null; failure=""; abort(); notifyAll(); }
    synchronized void stop() { desired=null;failedRequest=null; abort(); notifyAll(); }
    synchronized void networkChanged(){if(desired!=null)desired.retryAt=0;abort();notifyAll();}
    void abort() {
        java.net.Socket socket=opening; if(socket!=null) try{socket.close();}catch(IOException ignored){}
        Remote remote=active; if(remote!=null) try{remote.abort();}catch(IOException ignored){}
    }
    boolean current(Request request) { return !closed && desired==request; }
    void loop() {
        while(!closed) {
            Request request;
            synchronized(this) { while(!closed && desired==null) try{wait();}catch(InterruptedException ignored){} if(closed) return; request=desired; }
            synchronized(this){if(request.retryAt>SystemClock.elapsedRealtime()){try{wait(Math.min(1000,request.retryAt-SystemClock.elapsedRealtime()));}catch(InterruptedException ignored){}continue;}}
            try { consume(request); }
            catch(Exception error) {
                synchronized(this) {
                    if(current(request)) {
                        failure=error.getMessage()==null?error.getClass().getSimpleName():error.getMessage();String reason=failure;
                        if(RetryPolicy.transientFailure(error)&&request.attempts<RetryPolicy.MAX_ATTEMPTS){request.attempts++;request.retryAt=SystemClock.elapsedRealtime()+RetryPolicy.delayMillis(request.attempts);reconnects.incrementAndGet();main.post(()->{if(current(request))events.reconnecting(reason);});}
                        else{desired=null;failedRequest=request;main.post(()->{if(!closed&&desired==null&&failedRequest==request)events.failed(reason);});}
                    }
                }
            }
        }
    }
    void consume(Request request) throws Exception {
        Remote.Credentials c=request.credentials;
        AudioTrack track=null;OpusAudioDecoder decoder=null;
        long owner=request.timeline.beginAudio(),firstHost=0,firstFrame=0,lastPosition=0,positionEpoch=0;boolean anchored=false;
        AudioManager manager=(AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
        AudioAttributes attributes=new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build();
        AudioFocusRequest focus=new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN).setAudioAttributes(attributes)
            .setOnAudioFocusChangeListener(change -> {
                if(change==AudioManager.AUDIOFOCUS_LOSS || change==AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
                    if(current(request)) { stop(); events.failed("系统音频被其他应用中断，请重新开启"); }
                }
            },main).build();
        boolean focused=false;
        try(Remote remote=new Remote(c,16,4,socket -> {opening=socket; if(!current(request)) socket.close();})) {
            active=remote; if(!current(request)) return;
            DataInputStream config=Remote.reader(remote.call(41,Remote.body(o -> {o.writeInt(2);o.writeInt(OpusAudioDecoder.available()?1:0);})));
            if(config.readInt()!=2) throw new IOException("音频协议版本不兼容");
            int rate=config.readInt(), channels=config.readInt(), bits=config.readInt();
            String provider=Remote.str(config);
            int codec=config.readInt(),lookahead=config.readInt();
            if(codec<0||codec>1||lookahead<0||lookahead>5760||codec==1&&rate!=48000)throw new IOException("音频编码协商失败");
            if(codec==1)decoder=new OpusAudioDecoder(lookahead);encoding=codec==1?"Opus 96 kbps":"PCM16LE";wireBytes.set(0);
            if(rate<8000 || rate>96000 || rate%100!=0 || channels!=2 || bits!=16 || !provider.equals("wasapi-loopback") || config.available()!=0)
                throw new IOException("音频格式不兼容");
            int minimum=AudioTrack.getMinBufferSize(rate,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT);
            if(minimum<=0 || minimum>rate) throw new IOException("手机音频缓冲要求超过250ms");
            int buffer=Math.max(minimum,rate*4*80/1000);
            if(manager.requestAudioFocus(focus)!=AudioManager.AUDIOFOCUS_REQUEST_GRANTED) throw new IOException("无法获得手机音频播放权限");
            focused=true;
            track=new AudioTrack.Builder().setAudioAttributes(attributes)
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(buffer).setTransferMode(AudioTrack.MODE_STREAM).setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY).build();
            if(track.getState()!=AudioTrack.STATE_INITIALIZED) throw new IOException("AudioTrack初始化失败");
            if(!current(request)) return;
            readyRequest=request;
            failure="";
            sampleRate=rate; performanceMode=track.getPerformanceMode(); underruns=0; writtenFrames=playedFrames=firstPacketNs=0;
            packets.set(0); pcmBytes.set(0); nonZeroSamples.set(0); squaredSamples.set(0); sampleCount.set(0);
            tonePowerSum=0; analyzedPackets=0;
            boolean playing=false;long playStartedNs=0; AudioTimestamp timestamp=new AudioTimestamp();
            main.post(() -> { if(current(request)) events.started(rate); });
            remote.sendOnly(43,Remote.body(o -> o.writeInt(1)));
            long sequence=-1,lastPts=-1,basePts=0,baseClock=0;
            while(current(request)) {
                DataInputStream packet=Remote.reader(remote.receive(42));
                long next=packet.readLong(), pts=packet.readLong(); int flags=packet.readInt(); byte[] pcm=Remote.blob(packet,codec==1?1275:rate*4/100);
                if(next!=sequence+1 || pts<0 || pts<=lastPts || pts>Long.MAX_VALUE/1000 || (flags & ~3)!=0 || (codec==0 && pcm.length%4!=0) || ((flags&2)!=0)!= (pcm.length==0) || packet.available()!=0)
                    throw new IOException("音频序号、时间戳或长度错误");
                sequence=next; lastPts=pts; packets.incrementAndGet();
                if(sequence>=100)request.attempts=0;
                if(pcm.length==0) {
                    baseClock=0;
                    // Silence heartbeats carry no samples. A previous stream origin cannot
                    // bridge that gap: AudioTrack counts submitted samples, not absent time.
                    if(anchored){track.stop();track.flush();playing=false;writtenFrames=0;anchored=false;lastPosition=0;positionEpoch=0;request.timeline.endAudio(owner);owner=request.timeline.beginAudio();if(decoder!=null){decoder.hostOrigin=-1;decoder.decodedFrames=0;}}
                    continue;
                }
                wireBytes.addAndGet(pcm.length);
                if(decoder!=null){if((flags&1)!=0){decoder.hostOrigin=pts;decoder.decodedFrames=0;}pcm=decoder.decode(pcm,pts);if(pcm.length==0)continue;pts=decoder.outputHostPts;}
                if(!anchored){firstHost=pts;firstFrame=writtenFrames;anchored=true;}
                if((flags&1)!=0&&playing){track.stop();track.flush();playing=false;writtenFrames=0;firstHost=pts;firstFrame=0;lastPosition=0;positionEpoch=0;request.timeline.endAudio(owner);owner=request.timeline.beginAudio();}
                long now=System.nanoTime(); if(firstPacketNs==0) firstPacketNs=now;
                if(baseClock==0 || (flags&1)!=0) {baseClock=now; basePts=pts;}
                if(now-baseClock-(pts-basePts)*1000>250_000_000L) throw new IOException("音频网络积压超过250ms，请重连");
                long deadline=now+250_000_000L; int offset=0;
                while(offset<pcm.length && current(request)) {
                    int count=track.write(pcm,offset,pcm.length-offset,AudioTrack.WRITE_NON_BLOCKING);
                    if(count<0) throw new IOException("手机音频输出失败："+count);
                    offset+=count;
                    if(count==0) {if(System.nanoTime()>deadline) throw new IOException("手机音频输出阻塞"); Thread.sleep(2);}
                }
                pcmBytes.addAndGet(offset); writtenFrames+=offset/4;
                if(!playing && writtenFrames>=rate*60/1000) {playStartedNs=System.nanoTime();track.play(); playing=true;}
                if(measureSamples) {
                    long nonzero=0,squares=0,samples=0;
                    for(int i=0;i<offset;i+=2) { int value=(short)((pcm[i]&255)|(pcm[i+1]<<8)); if(value!=0) nonzero++; squares+=(long)value*value; samples++; }
                    if(sampleCount.get()>10_000_000) {nonZeroSamples.set(0);squaredSamples.set(0);sampleCount.set(0);}
                    nonZeroSamples.addAndGet(nonzero); squaredSamples.addAndGet(squares); sampleCount.addAndGet(samples);
                    double coefficient=2*Math.cos(2*Math.PI*997/rate),previous=0,older=0;
                    for(int i=0;i<offset;i+=4) { int value=(short)((pcm[i]&255)|(pcm[i+1]<<8)); double filterState=value+coefficient*previous-older; older=previous; previous=filterState; }
                    double frames=offset/4.0;
                    if(frames>0) {tonePowerSum+=(previous*previous+older*older-coefficient*previous*older)/(frames*frames); analyzedPackets++;}
                }
                underruns=track.getUnderrunCount();
                if(track.getTimestamp(timestamp)){long position=timestamp.framePosition&0xffffffffL;if(position<lastPosition&&lastPosition-position>0x80000000L)positionEpoch+=0x100000000L;lastPosition=position;playedFrames=positionEpoch+position;
                    if(playing&&anchored&&playedFrames>=firstFrame&&timestamp.nanoTime>=playStartedNs&&timestamp.nanoTime<=System.nanoTime()+10_000_000L)request.timeline.anchor(owner,firstHost+(playedFrames-firstFrame)*1_000_000L/rate,timestamp.nanoTime);
                }
            }
        } finally {
            request.timeline.endAudio(owner);
            if(decoder!=null)try{decoder.close();}catch(Exception ignored){}
            try { if(track!=null) {try{track.pause();track.flush();}finally{track.release();}} }
            finally {
                try {if(focused) manager.abandonAudioFocusRequest(focus);}
                finally {active=null; opening=null; readyRequest=null;}
            }
        }
    }
    void close() { closed=true; stop(); }
}
