package org.lantern.client;
import javax.crypto.*;
import javax.crypto.spec.*;
import java.nio.*;
import java.security.*;
import java.util.*;
/** AES-256-GCM, fixed authenticated header and fresh directional TLS session keys. Single owner. */
final class DatagramCrypto {
    static final int PAYLOAD=1024,HEADER=56,TAG=16,MAX_FRAME=16*1024*1024;
    static final int DATA=1,PARITY=2,NACK=3,HELLO=4,CHALLENGE=5,PROOF=6,FEEDBACK=7;
    static final class Packet {
        int type,index,count,total;long sequence,frame;byte[] payload;
        Packet(int t,long f,int i,int n,int size,byte[] bytes){type=t;frame=f;index=i;count=n;total=size;payload=bytes;}
    }
    final SecretKeySpec key;final byte[] session;final int direction;
    final Cipher cipher;long sent,highest;final long[] seen=new long[2048];
    DatagramCrypto(byte[] key,byte[] session,int direction)throws GeneralSecurityException{
        if(key.length!=32||session.length!=16||direction<1)throw new GeneralSecurityException("UDP key bound");
        this.key=new SecretKeySpec(key,"AES");this.session=session.clone();this.direction=direction;cipher=Cipher.getInstance("AES/GCM/NoPadding");
    }
    byte[] seal(Packet p)throws GeneralSecurityException {
        if(p.payload.length>PAYLOAD||sent==Long.MAX_VALUE)throw new GeneralSecurityException("UDP sender bound");
        long seq=++sent;ByteBuffer header=ByteBuffer.allocate(HEADER).order(ByteOrder.BIG_ENDIAN);
        header.putInt(0x4c554433).putShort((short)1).putShort((short)p.type).put(session).putLong(seq).putLong(p.frame)
            .putInt(p.index).putInt(p.count).putInt(p.total).putInt(p.payload.length);
        byte[] nonce=ByteBuffer.allocate(12).putInt(direction).putLong(seq).array();
        cipher.init(Cipher.ENCRYPT_MODE,key,new GCMParameterSpec(128,nonce));cipher.updateAAD(header.array());
        byte[] encrypted=cipher.doFinal(p.payload);byte[] out=Arrays.copyOf(header.array(),HEADER+encrypted.length);
        System.arraycopy(encrypted,0,out,HEADER,encrypted.length);return out;
    }
    Packet open(byte[] bytes,int length){
        if(length<HEADER+TAG||length>HEADER+TAG+PAYLOAD||length>bytes.length)return null;
        try{
            ByteBuffer b=ByteBuffer.wrap(bytes,0,length).order(ByteOrder.BIG_ENDIAN);
            if(b.getInt()!=0x4c554433||b.getShort()!=1)return null;int type=b.getShort()&65535;
            if(type<1||type>7)return null;byte[] id=new byte[16];b.get(id);if(!MessageDigest.isEqual(id,session))return null;
            long seq=b.getLong(),frame=b.getLong();int index=b.getInt(),count=b.getInt(),total=b.getInt(),size=b.getInt();
            if(seq<=0||size<0||size>PAYLOAD||length!=HEADER+size+TAG||
               (seq<=highest&&(highest-seq>=seen.length||seen[(int)(seq&2047)]==seq)))return null;
            byte[] nonce=ByteBuffer.allocate(12).putInt(direction).putLong(seq).array();
            cipher.init(Cipher.DECRYPT_MODE,key,new GCMParameterSpec(128,nonce));cipher.updateAAD(bytes,0,HEADER);
            byte[] clear=cipher.doFinal(bytes,HEADER,size+TAG);
            highest=Math.max(highest,seq);seen[(int)(seq&2047)]=seq;
            Packet result=new Packet(type,frame,index,count,total,clear);result.sequence=seq;return result;
        }catch(GeneralSecurityException|RuntimeException invalid){return null;}
    }
    static final class Frames {
        static final class Frame {
            final int count,total;final long began;
            final TreeMap<Integer,byte[]> data=new TreeMap<>(),parity=new TreeMap<>();
            Frame(Packet p,long now){count=p.count;total=p.total;began=now;}
        }
        final TreeMap<Long,Frame> frames=new TreeMap<>();long delivered=-1;
        byte[] accept(Packet p,long now){
            if((p.type!=DATA&&p.type!=PARITY)||p.frame<=delivered||p.total<=0||p.total>MAX_FRAME||p.count!=(p.total+PAYLOAD-1)/PAYLOAD||p.index<0)return null;
            if(p.type==DATA&&(p.index>=p.count||p.payload.length!=Math.min(PAYLOAD,p.total-p.index*PAYLOAD))||
               p.type==PARITY&&(p.index>=(p.count+7)/8||p.payload.length!=PAYLOAD))return null;
            frames.entrySet().removeIf(e->now-e.getValue().began>200);
            Frame frame=frames.get(p.frame);
            if(frame==null){if(frames.size()>=2)frames.pollFirstEntry();frame=new Frame(p,now);frames.put(p.frame,frame);}
            if(frame.count!=p.count||frame.total!=p.total)return null;
            (p.type==DATA?frame.data:frame.parity).putIfAbsent(p.index,p.payload);
            for(Map.Entry<Integer,byte[]> e:frame.parity.entrySet()){
                int absent=0,index=0;
                for(int i=e.getKey()*8;i<Math.min(frame.count,e.getKey()*8+8);i++)if(!frame.data.containsKey(i)){absent++;index=i;}
                if(absent==1){byte[] recovered=e.getValue().clone();
                    for(int i=e.getKey()*8;i<Math.min(frame.count,e.getKey()*8+8);i++)if(i!=index){byte[] part=frame.data.get(i);for(int j=0;j<part.length;j++)recovered[j]^=part[j];}
                    frame.data.put(index,Arrays.copyOf(recovered,Math.min(PAYLOAD,frame.total-index*PAYLOAD)));
                }
            }
            if(frame.data.size()!=frame.count)return null;
            byte[] out=new byte[frame.total];for(Map.Entry<Integer,byte[]> e:frame.data.entrySet())System.arraycopy(e.getValue(),0,out,e.getKey()*PAYLOAD,e.getValue().length);
            delivered=p.frame;frames.headMap(delivered,true).clear();return out;
        }
        int[] missing(long id,long now){Frame frame=frames.get(id);if(frame==null||now-frame.began<15||now-frame.began>160)return new int[0];
            int[] out=new int[Math.min(128,frame.count)];int n=0;for(int i=0;i<frame.count&&n<out.length;i++)if(!frame.data.containsKey(i))out[n++]=i;return Arrays.copyOf(out,n);}
    }
}
