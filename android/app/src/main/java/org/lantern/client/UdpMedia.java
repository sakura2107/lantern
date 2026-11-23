package org.lantern.client;
import java.io.*;
import java.net.*;
import java.security.*;
import java.util.*;
/** One owning media worker, close may interrupt it. No socket reuse across authenticated sessions. */
final class UdpMedia implements AutoCloseable {
    static final class Offer {
        final int port;final byte[] session,sendKey,receiveKey;
        Offer(DataInputStream r)throws IOException{port=r.readInt();session=Remote.blob(r,16);sendKey=Remote.blob(r,32);receiveKey=Remote.blob(r,32);
            if(port<1||port>65535||session.length!=16||sendKey.length!=32||receiveKey.length!=32)throw new IOException("UDP offer bound");}
        void clear(){Arrays.fill(sendKey,(byte)0);Arrays.fill(receiveKey,(byte)0);}
    }
    final DatagramSocket socket;final DatagramCrypto outgoing,incoming;final DatagramCrypto.Frames frames=new DatagramCrypto.Frames();
    final byte[] buffer=new byte[1200];final DatagramPacket receivePacket=new DatagramPacket(buffer,buffer.length);volatile boolean closed;long lastFeedback,lastDelivered;
    UdpMedia(String host,Offer offer)throws Exception{
        outgoing=new DatagramCrypto(offer.receiveKey,offer.session,2);incoming=new DatagramCrypto(offer.sendKey,offer.session,1);offer.clear();
        socket=new DatagramSocket();try{socket.connect(InetAddress.getByName(host),offer.port);socket.setSoTimeout(5);}catch(Exception e){socket.close();throw e;}
    }
    static long now(){return System.nanoTime()/1_000_000;}
    void send(int type,long frame,byte[] payload)throws Exception{
        byte[] bytes=outgoing.seal(new DatagramCrypto.Packet(type,frame,0,0,0,payload));socket.send(new DatagramPacket(bytes,bytes.length));
    }
    DatagramCrypto.Packet read()throws IOException{
        receivePacket.setLength(buffer.length);
        try{socket.receive(receivePacket);return incoming.open(buffer,receivePacket.getLength());}catch(SocketTimeoutException timeout){return null;}
    }
    boolean handshake()throws Exception{
        long end=now()+1500,lastHello=0;SecureRandom random=new SecureRandom();
        while(!closed&&now()<end){
            if(now()-lastHello>=300){byte[] nonce=new byte[32];random.nextBytes(nonce);send(DatagramCrypto.HELLO,0,nonce);lastHello=now();}
            DatagramCrypto.Packet p=read();if(p==null)continue;
            if(p.type==DatagramCrypto.CHALLENGE&&p.payload.length==32)send(DatagramCrypto.PROOF,0,p.payload);
            else if(p.type==DatagramCrypto.FEEDBACK&&Arrays.equals(p.payload,new byte[]{1}))return true;
        }return false;
    }
    byte[] receive(int fps)throws Exception{
        int retentionMs=Math.min(160,2000/Math.max(fps,1));
        long end=now()+1500,lastNack=0;
        while(!closed&&now()<end){
            DatagramCrypto.Packet p=read();long now=now();
            if(now-lastFeedback>=100){send(DatagramCrypto.FEEDBACK,lastDelivered,new byte[]{1});lastFeedback=now;}
            if(p!=null&&(p.type==DatagramCrypto.DATA||p.type==DatagramCrypto.PARITY)){
                long current=p.frame;byte[] bytes=frames.accept(p,now);if(bytes!=null){lastDelivered=current;send(DatagramCrypto.FEEDBACK,current,new byte[]{1});lastFeedback=now;return bytes;}
            }
            if(now-lastNack>=5){
                for(long frame:frames.frames.descendingKeySet()){
                    DatagramCrypto.Frames.Frame pending=frames.frames.get(frame);
                    if(pending.lastNack!=0 && now-pending.lastNack<20)continue;
                    int[] missing=frames.missing(frame,now,retentionMs);
                    if(missing.length==0)continue;
                    byte[] payload=Remote.body(o->{o.writeInt(missing.length);for(int index:missing)o.writeInt(index);});
                    send(DatagramCrypto.NACK,frame,payload);
                    pending.lastNack=now;
                    break;
                }
                lastNack=now;
            }
        }
        throw new SocketTimeoutException("UDP media timed out");
    }
    public void close(){closed=true;socket.close();}
}
