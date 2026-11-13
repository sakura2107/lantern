package org.lantern.client;
import java.io.*;
import java.util.*;
/** Versioned desktop protocol backend. Owner supplies the stream Remote and owns its cancellation. */
final class DesktopSession {
    static final class Status {
        final int width,height,fps,bitrate,selectedWidth;final String codec,path;
        final boolean constrained,dropping;
        Status(int w,int h,int f,int b,String c,String p,int selected,boolean limited,boolean drop){width=w;height=h;fps=f;bitrate=b;codec=c;path=p;selectedWidth=selected;constrained=limited;dropping=drop;}
    }
    static final class Display {
        final int index,width,height,refresh;final String name;final boolean avc,hevc;
        Display(DataInputStream r)throws IOException{index=r.readInt();width=r.readInt();height=r.readInt();refresh=r.readInt();name=Remote.str(r);avc=r.readInt()==1;hevc=r.readInt()==1;}
    }
    final Remote remote;final Remote.Credentials credentials;final int version;
    final String id,codec,capture,encoder;
    volatile Status status;
    byte[] headers;volatile UdpMedia udp;boolean usingUdp;
    boolean automatic;
    DesktopSession(Remote r,Remote.Credentials c,AvcDesktop.Profile p)throws Exception {
        this(r,c,p,true);
    }
    DesktopSession(Remote r,Remote.Credentials c,AvcDesktop.Profile p,boolean preferHevc)throws Exception {
        remote=r;automatic=p.automatic;credentials=new Remote.Credentials(c.host,c.port,c.pin,c.token,c.endpoints);
        DataInputStream capsReader=Remote.reader(r.call(30,new byte[0]));String caps=Remote.str(capsReader);
        if(capsReader.available()!=0)throw new IOException("Capability trailing bytes");
        version=caps.contains(";decoder-table=v1")?5:caps.contains(";datagram=av-v1")?4:caps.contains(";desktop-quality=v3")?3:caps.contains("av-clock=host-qpc-v1")?2:1;
        DecoderCapabilities.Limit limit=null;long capacity=0;
        if(version>=3){
            List<Display> displays=limits(credentials);
            if(p.monitor<0||p.monitor>=displays.size())throw new IOException("Monitor index");
            Display display=displays.get(p.monitor);
            // Advertise the hardware ceiling independently of the initial user setting, so 30 -> 120
            // can be reconfigured on this same connection.
            limit=DecoderCapabilities.best(preferHevc&&display.hevc,Math.min(3840,display.width),Math.min(2160,display.height),120);
            if(limit==null)throw new IOException("没有兼容的硬件视频解码器");
            if(p.automatic){long began=System.nanoTime();DataInputStream probe=Remote.reader(r.call(60,Remote.body(o->o.writeInt(1))));
                byte[] bytes=Remote.blob(probe,262144);if(probe.available()!=0)throw new IOException("Probe trailing bytes");
                capacity=(long)(bytes.length*8.0/Math.max(.001,(System.nanoTime()-began)/1e9));}
        }
        final DecoderCapabilities.Limit selected=limit;final long bandwidth=capacity;
        final List<DecoderCapabilities.Limit> modes=new ArrayList<>();
        if(version==5)for(DecoderCapabilities.Limit mode:DecoderCapabilities.query())
            if(mode.mime.equals(selected.mime)){
                boolean duplicate=false;for(DecoderCapabilities.Limit seen:modes)if(seen.width==mode.width&&seen.height==mode.height&&seen.fps==mode.fps)duplicate=true;
                if(!duplicate)modes.add(mode);
            }
        if(version==5&&(modes.isEmpty()||modes.size()>64))throw new IOException("Decoder mode count");
        DataInputStream config=Remote.reader(r.call(32,Remote.body(o->{
            o.writeInt(version);o.writeInt(p.monitor);o.writeInt(version>=3?p.width:Math.min(1920,p.width));
            o.writeInt(version>=3?p.fps:Math.min(60,p.fps));o.writeInt(Math.max(2_000_000,Math.min(30_000_000,p.bitrate)));
            if(version>=3){o.writeInt(selected.mime.equals("video/hevc")?1:0);o.writeInt(selected.width);o.writeInt(selected.height);o.writeInt(selected.fps);o.writeInt(p.automatic?1:0);o.writeLong(bandwidth);}
            if(version==5){o.writeInt(modes.size());for(DecoderCapabilities.Limit mode:modes){o.writeInt(mode.width);o.writeInt(mode.height);o.writeInt(mode.fps);}}
        })));
        if(config.readInt()!=version)throw new IOException("Desktop version mismatch");
        int w=config.readInt(),h=config.readInt(),f=config.readInt(),b=config.readInt();
        capture=Remote.str(config);encoder=Remote.str(config);headers=Remote.blob(config,65536);
        if(version>=2&&!Remote.str(config).equals("host-qpc-us"))throw new IOException("Desktop clock domain");
        id=version>=3?Remote.str(config):"";codec=version>=3?Remote.str(config):"h264";
        if(version>=4){UdpMedia.Offer offer=new UdpMedia.Offer(config);try{udp=new UdpMedia(r.selectedEndpoint.host,offer);}catch(Exception unavailable){offer.clear();}}
        validate(w,h,f,b);
        if(config.available()!=0||(version>=3&&!id.matches("[0-9a-f]{64}"))||(!codec.equals("h264")&&!codec.equals("hevc")))throw new IOException("Desktop configuration");
        status=new Status(w,h,f,b,codec,"tcp",w,false,false);
    }
    static void validate(int w,int h,int f,int b)throws IOException {
        if(w<160||w>3840||h<1||h>2160||f<1||f>120||b<250000||b>160000000)throw new IOException("Desktop codec bounds");
    }
    /** Reads v3 metadata before the encoded frame. Caller reconfigures decoder on dimensions changing. */
    Status metadata(DataInputStream packet)throws IOException {
        if(version<3)return status;
        int w=packet.readInt(),h=packet.readInt(),f=packet.readInt(),b=packet.readInt();String c=Remote.str(packet);
        byte[] csd=Remote.blob(packet,65536);int selected=packet.readInt(),limited=packet.readInt(),drop=packet.readInt();validate(w,h,f,b);
        if(!c.equals(codec)||limited<0||limited>1||drop<0||drop>1)throw new IOException("Desktop metadata");
        if(csd.length>0)headers=csd;
        return status=new Status(w,h,f,b,c,usingUdp?"udp":"tcp",selected,limited==1,drop==1);
    }
    void ready()throws Exception {
        boolean requested=false;
        if(udp!=null)try{requested=udp.handshake();}catch(Exception unavailable){}
        final boolean want=requested;
        remote.sendOnly(34,Remote.body(o->{o.writeInt(1);if(version>=4)o.writeInt(want?1:0);}));
        if(version>=4){DataInputStream path=Remote.reader(remote.receive(61));String value=Remote.str(path);
            if(path.available()!=0||(!value.equals("udp")&&!value.equals("tcp")))throw new IOException("Media path");usingUdp=value.equals("udp");}
    }
    byte[] packet()throws Exception {
        if(usingUdp){
            try{return Native.response(33,udp.receive());}
            catch(java.net.SocketException|java.net.SocketTimeoutException unavailable){
                udp.close();usingUdp=false;DataInputStream path=Remote.reader(remote.receive(61));
                if(!Remote.str(path).equals("tcp")||path.available()!=0)throw new IOException("TCP fallback path");
            }
        }
        return remote.receive(33);
    }
    void close(){if(udp!=null)udp.close();}
    synchronized void setQuality(AvcDesktop.Profile p,boolean keyframe)throws Exception {
        if(version<3)throw new IOException("旧主机不支持在线画质调整");
        try(Remote control=new Remote(credentials,4,0)) {
            long measured=0;
            if(p.automatic&&!automatic){long start=System.nanoTime();byte[] sample=Remote.blob(Remote.reader(control.call(60,Remote.body(o->o.writeInt(1)))),262144);
                measured=(long)(sample.length*8.0/Math.max(.001,(System.nanoTime()-start)/1e9));}
            final long capacity=measured;
            control.call(59,Remote.body(o->{o.writeInt(1);Remote.str(o,id);o.writeInt(p.width);o.writeInt(p.fps);o.writeInt(p.automatic?1:0);o.writeInt(keyframe?1:0);o.writeLong(capacity);}));
            automatic=p.automatic;
        }
    }
    static List<Display> limits(Remote.Credentials c)throws Exception {
        try(Remote remote=new Remote(c,4,0)){
            DataInputStream r=Remote.reader(remote.call(58,Remote.body(o->o.writeInt(1))));
            if(r.readInt()!=1)throw new IOException("Display limits version");int n=r.readInt();if(n<0||n>64)throw new IOException("Display count");
            List<Display> out=new ArrayList<>();for(int i=0;i<n;i++)out.add(new Display(r));if(r.available()!=0)throw new IOException("Display trailing bytes");return out;
        }
    }
}
