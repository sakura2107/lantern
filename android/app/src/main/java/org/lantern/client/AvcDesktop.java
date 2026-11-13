package org.lantern.client;

import android.media.*;
import android.os.*;
import java.io.*;
import java.nio.*;
import java.util.*;
import java.util.concurrent.atomic.*;

/** One network/codec worker, one encoded packet in flight. Compressed bytes never become Java pixels. */
final class AvcDesktop implements AutoCloseable {
    static final class Profile {
        final int width, fps, bitrate,monitor;final boolean automatic;
        Profile(int w, int f, int b) {this(w,f,b,0);}
        Profile(int w,int f,int b,int m) {this(w,f,b,m,false);}
        Profile(int w,int f,int b,int m,boolean auto) {
            automatic=auto;
            monitor=m;
            width = w;
            fps = f;
            bitrate = b;
        }
    }
    interface Events {
        void configured(int width, int height, String decoder, boolean hardware);
        void frame();
        void failed(String reason,boolean fallback);
        void reconnecting(String reason);
        default void syncWarning(){}
        default void quality(DesktopSession.Status status){}
        default void connectionState(ConnectionStatus.Snapshot state){}
    }
    static final class Request {
        final Remote.Credentials credentials;
        volatile Profile profile;
        MediaTimeline timeline;boolean absolute;boolean syncWarned;
        int attempts;long retryAt;
        Request(Remote.Credentials c, Profile p) {
            credentials = c;
            profile = p;
        }
    }
    java.util.function.Supplier<MediaTimeline> timelineSource=MediaTimeline::new;
    final AvcView view;
    final Events events;
    final ConnectionStatus connection=new ConnectionStatus();
    ConnectionStatus.Snapshot connectionState(){return connection.snapshot();}
    final Handler main = new Handler(Looper.getMainLooper());
    final HandlerThread notifications = new HandlerThread("Lantern codec notifications");
    final Handler codecEvents;
    final Thread worker;
    final AtomicBoolean framePosted = new AtomicBoolean();
    final AtomicBoolean render = new AtomicBoolean(true);
    volatile Remote active;
    volatile DesktopSession activeSession;
    final java.util.concurrent.ThreadPoolExecutor controls=new java.util.concurrent.ThreadPoolExecutor(
        1,1,0,java.util.concurrent.TimeUnit.MILLISECONDS,new java.util.concurrent.ArrayBlockingQueue<Runnable>(1),
        r->new Thread(r,"Lantern desktop settings"),new java.util.concurrent.ThreadPoolExecutor.DiscardOldestPolicy());
    void changeQuality(DesktopSession session,Profile profile,boolean idr){
        controls.execute(()->{try{if(activeSession==session)session.setQuality(profile,idr);}catch(Exception error){
            failure="画质调整失败："+error.getClass().getSimpleName();}});
    }
    volatile java.net.Socket openingSocket;
    volatile Request desired;
    volatile boolean closed;
    Request failed;
    volatile String decoderName = "", failure = "";
    volatile boolean hardwareDecoder;
    boolean preferHevc=true; // Set before start; permits an explicit compatibility fallback.
    volatile int width, height;
    final AtomicLong received = new AtomicLong(), repeated = new AtomicLong(),
                     encodedBytes = new AtomicLong();
    final AtomicLong reconnects=new AtomicLong();
    final AtomicLong streamEpoch=new AtomicLong();
    final long[] renderTimes = new long[768];
    long rendered, firstFrameNs, startedNs;
    AvcDesktop(AvcView v, Events e) {
        view = v;
        events = e;
        notifications.start();
        codecEvents = new Handler(notifications.getLooper());
        worker = new Thread(this::loop, "Lantern H264 desktop");
        worker.start();
        view.changed = () -> {synchronized (this){notifyAll();
    }
};
}
synchronized void start(Remote.Credentials c, Profile p) {
    if (closed || (desired != null && desired.credentials == c && desired.profile == p) ||
        (failed != null && failed.credentials == c && failed.profile == p))
        return;
    if(desired!=null&&desired.credentials==c&&desired.profile.monitor==p.monitor&&activeSession!=null&&activeSession.version>=3){
        desired.profile=p;changeQuality(activeSession,p,false);return;
    }
    desired = new Request(c, p);desired.timeline=timelineSource.get();
    connection.failure(false);
    failed = null;
    failure = "";
    closeNetwork();
    notifyAll();
}
synchronized void stop() {
    desired = null;
    failed = null;
    closeNetwork();
    notifyAll();
}
synchronized void networkChanged(){if(desired!=null)desired.retryAt=0;closeNetwork();notifyAll();}
void closeNetwork() {
    DesktopSession session=activeSession;if(session!=null)session.close();
    java.net.Socket opening = openingSocket;
    if (opening != null) try { opening.close(); } catch (IOException ignored) {}
    Remote r = active;
    if (r != null)
        try {
            r.abort();
        } catch (IOException ignored) {
        }
}
boolean current(Request r, AvcView.Target target) {
    return !closed && desired == r && view.isCurrent(target);
}
void loop() {
    while (!closed) {
        Request request;
        AvcView.Target target;
        synchronized (this) {
            while (!closed && (desired == null))
                try {
                    wait();
                } catch (InterruptedException ignored) {
                }
            if (closed)
                break;
            request = desired;
            if(request.retryAt>SystemClock.elapsedRealtime()){
                try{wait(Math.min(1000,request.retryAt-SystemClock.elapsedRealtime()));}catch(InterruptedException ignored){}
                continue;
            }
        }
        target = view.acquire();
        if (target == null) {
            synchronized (this) {
                try {
                    wait(100);
                } catch (InterruptedException ignored) {
                }
            }
            continue;
        }
        try {
            consume(request, target);
        } catch (Exception error) {
            synchronized (this) {
                if (desired == request && view.isCurrent(target) && !closed) {
                    failure =
                        error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage();
                    connection.failure(!RetryPolicy.transientFailure(error));
                    main.post(()->{if(!closed&&desired==request)events.connectionState(connection.snapshot());});
                    if(RetryPolicy.transientFailure(error)){
                        request.attempts=Math.min(1000,request.attempts+1);request.retryAt=SystemClock.elapsedRealtime()+Math.min(2000,RetryPolicy.delayMillis(request.attempts));reconnects.incrementAndGet();String reason=failure;
                        main.post(()->{if(!closed&&desired==request)events.reconnecting(reason);});
                        continue;
                    }
                    failed = request;
                    desired = null;
                    String message = failure;
                    main.post(() -> {
                        if (!closed && failed == request && desired == null)
                            events.failed(message,!RetryPolicy.transientFailure(error)&&!(error instanceof Remote.TransportError)&&!(error instanceof SecurityException)&&!(error instanceof javax.net.ssl.SSLException)&&!(error instanceof Remote.RemoteError&&((Remote.RemoteError)error).code!=6));
                    });
                }
            }
        } finally {
            target.release();
        }
    }
}
static byte[][] parameterSets(byte[] data) throws IOException {
    byte[] sps = null, pps = null;
    for (int i = 0; i + 4 < data.length;) {
        int prefix = data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1                       ? 3
                     : data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1 ? 4
                                                                                                : 0;
        if (prefix == 0) {
            i++;
            continue;
        }
        int end = i + prefix + 1;
        while (end + 3 < data.length && !(data[end] == 0 && data[end + 1] == 0 &&
                                          (data[end + 2] == 1 || (data[end + 2] == 0 && data[end + 3] == 1))))
            end++;
        if (end + 3 >= data.length)
            end = data.length;
        int type = data[i + prefix] & 31;
        if (type == 7)
            sps = Arrays.copyOfRange(data, i, end);
        if (type == 8)
            pps = Arrays.copyOfRange(data, i, end);
        i = end;
    }
    if (sps == null || pps == null)
        throw new IOException("H264 SPS/PPS missing");
    return new byte[][] {sps, pps};
}
void consume(Request request, AvcView.Target target) throws Exception {
    MediaCodec codec = null;
    final long epoch=streamEpoch.incrementAndGet();
    Remote.Credentials c=request.credentials;
    try (Remote remote = new Remote(new Remote.Credentials(c.host,c.port,c.pin,c.token,c.endpoints), 4, 1, socket -> { openingSocket = socket; if (!current(request, target)) socket.close(); })) {
        active = remote;
        if (!current(request, target))
            return;
        synchronized (this) {
            rendered = 0;
            firstFrameNs = 0;
            startedNs = System.nanoTime();
        }
        received.set(0);
        repeated.set(0);
        encodedBytes.set(0);
        DesktopSession session=new DesktopSession(remote,request.credentials,request.profile,preferHevc);
        activeSession=session;request.absolute=session.version>=2;
        DesktopSession.Status format=session.status;
        codec=createDecoder(request,target,epoch,format,session.headers);
        failure="";
        session.ready();
        long sequence = -1, lastPts = -1, clock = 0;
        boolean waitingKeyframe=false;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (current(request, target)) {
            DataInputStream packet = Remote.reader(session.packet());
            long next = packet.readLong(), pts = packet.readLong();
            int flags = packet.readInt();
            DesktopSession.Status updated=session.metadata(packet);
            byte[] data = Remote.blob(packet, session.version>=3?16*1024*1024-512:Remote.MAX-256);
            if(updated.width!=format.width||updated.height!=format.height||updated.fps!=format.fps){
                codec.stop();codec.release();codec=null;
                codec=createDecoder(request,target,epoch,updated,session.headers);format=updated;
            }
            if(session.version>=3)main.post(()->{if(current(request,target))events.quality(updated);});
            boolean gap=next!=sequence+1;
            if(session.version==4&&gap&&!waitingKeyframe){changeQuality(session,request.profile,true);waitingKeyframe=true;}
            if ((session.version<4&&gap) || next<=sequence || pts < 0 || pts > Long.MAX_VALUE / 1000 || pts <= lastPts ||
                (flags & ~3) != 0 || data.length == 0 || packet.available() != 0)
                throw new IOException("Desktop sequence, timestamp or payload invalid");
            sequence = next;
            if(sequence>=request.profile.fps)request.attempts=0;
            lastPts = pts;
            long now = System.nanoTime();
            if (clock == 0)
                clock = now - pts * 1000;
            if (now - (clock + pts * 1000) > 300_000_000L+(request.absolute?request.timeline.videoDelayBudget(now):0)) {
                clock=now-pts*1000;
                if(!waitingKeyframe&&session.version>=3)changeQuality(session,request.profile,true);
                waitingKeyframe=true;
            }
            if(waitingKeyframe){if((flags&1)==0)continue;codec.flush();waitingKeyframe=false;}
            received.incrementAndGet();
            encodedBytes.addAndGet(data.length);
            if ((flags & 2) != 0)
                repeated.incrementAndGet();
            int input = -1;
            long deadline = now + 500_000_000L;
            while (input < 0 && current(request, target) && System.nanoTime() < deadline) {
                drain(codec, info, target,request);
                input = codec.dequeueInputBuffer(10000);
            }
            if (!current(request, target))
                break;
            if (input < 0){
                codec.flush();waitingKeyframe=true;
                if(session.version>=3)changeQuality(session,request.profile,true);
                continue;
            }
            ByteBuffer buffer = codec.getInputBuffer(input);
            if (buffer == null || buffer.capacity() < data.length)
                throw new IOException("H264 input buffer bound");
            buffer.clear();
            buffer.put(data);
            codec.queueInputBuffer(input, 0, data.length, pts, 0);
            drain(codec, info, target,request);
        }
    } finally {
        streamEpoch.compareAndSet(epoch,epoch+1);
        if (codec != null) {
            try {
                codec.stop();
            } catch (Exception ignored) {
            }
            codec.release();
        }
        if(activeSession!=null)activeSession.close();activeSession=null;
        active = null;
        openingSocket = null;
    }
}
MediaCodec createDecoder(Request request,AvcView.Target target,long epoch,DesktopSession.Status status,byte[] headers)throws Exception {
        int w=status.width,h=status.height,fps=status.fps;
        String mime=status.codec.equals("hevc")?"video/hevc":"video/avc";
        MediaFormat format=MediaFormat.createVideoFormat(mime,w,h);
        if(mime.equals("video/hevc"))format.setByteBuffer("csd-0",ByteBuffer.wrap(headers));
        else{byte[][] sets=parameterSets(headers);format.setByteBuffer("csd-0",ByteBuffer.wrap(sets[0]));format.setByteBuffer("csd-1",ByteBuffer.wrap(sets[1]));}
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE,16*1024*1024);
        format.setInteger(MediaFormat.KEY_PRIORITY,0);
        String selected=null;
        for(MediaCodecInfo info:new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()){
            if(info.isEncoder()||(Build.VERSION.SDK_INT>=29&&!info.isHardwareAccelerated()))continue;
            boolean supported=Arrays.asList(info.getSupportedTypes()).contains(mime);if(!supported)continue;
            MediaCodecInfo.CodecCapabilities caps=info.getCapabilitiesForType(mime);
            if(!caps.getVideoCapabilities().areSizeAndRateSupported(w,h,fps))continue;
            selected=info.getName();
            if(Build.VERSION.SDK_INT>=30&&caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency))format.setInteger(MediaFormat.KEY_LOW_LATENCY,1);
            break;
        }
        if(selected==null)throw new IOException("没有兼容分辨率和帧率的硬件解码器");
        MediaCodec codec=MediaCodec.createByCodecName(selected);
        try{
            decoderName=codec.getCodecInfo().getName();hardwareDecoder=Build.VERSION.SDK_INT>=29&&codec.getCodecInfo().isHardwareAccelerated();
            width=w;height=h;codec.configure(format,target.surface,null,0);
        codec.setOnFrameRenderedListener((c, pts, nano) -> {
            if (!current(request, target)||streamEpoch.get()!=epoch)
                return;
            if(connection.snapshot().phase!=ConnectionStatus.Phase.NORMAL){connection.normal();main.post(()->{if(current(request,target))events.connectionState(connection.snapshot());});}
            synchronized (this) {
                renderTimes[(int)(rendered++ % renderTimes.length)] = nano;
                if(request.absolute)request.timeline.observe(pts,nano);
                if (firstFrameNs == 0)
                    firstFrameNs = System.nanoTime();
            }
            if (framePosted.compareAndSet(false, true))
                main.post(() -> {
                    framePosted.set(false);
                    if (current(request, target)&&streamEpoch.get()==epoch)
                        events.frame();
                });
        }, codecEvents);
        codec.start();
            main.post(()->{if(current(request,target)&&streamEpoch.get()==epoch)events.configured(w,h,decoderName,hardwareDecoder);});
            return codec;
        }catch(Exception error){codec.release();throw error;}
}
void drain(MediaCodec codec, MediaCodec.BufferInfo info, AvcView.Target target,Request request) {
    for (int i = 0; i < 32; i++) {
        int output = codec.dequeueOutputBuffer(info, 0);
        if (output >= 0){
            boolean visible=render.get()&&target.visible&&current(request,target);
            long now=System.nanoTime(),at=request.absolute?request.timeline.deadline(info.presentationTimeUs,now):now;
            if(request.absolute&&request.timeline.fallbacks>0&&!request.syncWarned){request.syncWarned=true;main.post(()->{if(current(request,target))events.syncWarning();});}
            if(!visible||at<0)codec.releaseOutputBuffer(output,false);
            else if(at>now)codec.releaseOutputBuffer(output,at);
            else codec.releaseOutputBuffer(output,true);
        }
        else if (output == MediaCodec.INFO_TRY_AGAIN_LATER)
            break;
    }
}
synchronized long[] renderedTimes() {
    int n = (int)Math.min(rendered, renderTimes.length);
    long[] result = new long[n];
    for (int i = 0; i < n; i++)
        result[i] = renderTimes[(int)((rendered - n + i) % renderTimes.length)];
    return result;
}
synchronized double firstFrameMs() {
    return firstFrameNs == 0 ? -1 : (firstFrameNs - startedNs) / 1e6;
}
@Override
public synchronized void close() {
    closed = true;
    desired = null;
    closeNetwork();
    notifyAll();
    worker.interrupt();
    notifications.quitSafely();
    controls.shutdownNow();
}
}
