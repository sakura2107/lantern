package org.lantern.client;
import java.io.*;
/** Audio protocol and transport backend, independent of AudioTrack and UI. */
final class AudioSession implements AutoCloseable {
    final Remote remote;final int version,rate,channels,bits,codec,lookahead;final String provider;
    volatile UdpMedia udp;volatile boolean usingUdp;
    AudioSession(Remote r,boolean opus)throws Exception{
        remote=r;String caps=Remote.str(Remote.reader(r.call(30,new byte[0])));version=caps.contains(";datagram=av-v1")?3:2;
        DataInputStream config=Remote.reader(r.call(41,Remote.body(o->{o.writeInt(version);o.writeInt(opus?1:0);} )));
        if(config.readInt()!=version)throw new IOException("Audio protocol version");
        rate=config.readInt();channels=config.readInt();bits=config.readInt();provider=Remote.str(config);codec=config.readInt();lookahead=config.readInt();
        if(version==3){UdpMedia.Offer offer=new UdpMedia.Offer(config);try{udp=new UdpMedia(r.selectedEndpoint.host,offer);}catch(Exception unavailable){offer.clear();}}
        if(config.available()!=0)throw new IOException("Audio config trailing bytes");
    }
    void ready()throws Exception{
        boolean requested=false;if(udp!=null)try{requested=udp.handshake();}catch(Exception unavailable){}
        final boolean want=requested;
        remote.sendOnly(43,Remote.body(o->{o.writeInt(1);if(version==3)o.writeInt(want?1:0);}));
        if(version==3){DataInputStream r=Remote.reader(remote.receive(61));String path=Remote.str(r);
            if(r.available()!=0||(!path.equals("udp")&&!path.equals("tcp")))throw new IOException("Audio path");usingUdp=path.equals("udp");}
    }
    byte[] packet()throws Exception{
        if(usingUdp)try{return Native.response(42,udp.receive());}
        catch(java.net.SocketException|java.net.SocketTimeoutException timeout){
            udp.close();usingUdp=false;DataInputStream path=Remote.reader(remote.receive(61));
            if(!Remote.str(path).equals("tcp")||path.available()!=0)throw new IOException("Audio TCP fallback");
        }
        return remote.receive(42);
    }
    public void close(){if(udp!=null)udp.close();}
}
