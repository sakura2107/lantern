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
            final int count,total;final long began;long lastNack;
            final TreeMap<Integer,byte[]> data=new TreeMap<>(),parity=new TreeMap<>();
            Frame(Packet p,long now){count=p.count;total=p.total;began=now;}
        }
        final TreeMap<Long,Frame> frames=new TreeMap<>();long delivered=-1;
        byte[] accept(Packet p,long now){
            if((p.type!=DATA&&p.type!=PARITY)||p.frame<=delivered||p.total<=0||p.total>MAX_FRAME||p.count!=(p.total+PAYLOAD-1)/PAYLOAD||p.index<0)return null;
            boolean isParity=p.type==PARITY;
            if(p.index>=(isParity?(p.count+7)/8:p.count))return null;
            int first=isParity?p.index*8:p.index;
            int length=Math.min(PAYLOAD,p.total-first*PAYLOAD);
            if(p.payload.length!=length && !(isParity && p.payload.length==PAYLOAD))return null;
            if(!frames.isEmpty()){
                long newest=Math.max(p.frame,frames.lastKey());
                if(newest-p.frame>=2)return null;
                while(!frames.isEmpty() && newest-frames.firstKey()>=2)frames.pollFirstEntry();
            }
            for(Frame old:frames.values())if(now-old.began>=200){old.data.clear();old.parity.clear();}
            Frame frame=frames.get(p.frame);
            if(frame==null){frame=new Frame(p,now);frames.put(p.frame,frame);}
            if(now-frame.began>=200||frame.count!=p.count||frame.total!=p.total)return null;
            boolean inserted=(p.type==DATA?frame.data:frame.parity).putIfAbsent(p.index,p.payload)==null;
            if(inserted){
                int group=p.type==DATA?p.index/8:p.index;
                byte[] parity=frame.parity.get(group);
                if(parity!=null){
                    int start=group*8,end=Math.min(frame.count,start+8),absent=0,index=0;
                    for(int i=start;i<end;i++)if(!frame.data.containsKey(i)){absent++;index=i;}
                    if(absent==1){
                        byte[] recovered=parity.clone();
                        for(int i=start;i<end;i++)if(i!=index){byte[] part=frame.data.get(i);for(int j=0;j<part.length;j++)recovered[j]^=part[j];}
                        frame.data.put(index,Arrays.copyOf(recovered,Math.min(PAYLOAD,frame.total-index*PAYLOAD)));
                    }
                }
            }
            if(frame.data.size()!=frame.count)return null;
            byte[] out=new byte[frame.total];for(Map.Entry<Integer,byte[]> e:frame.data.entrySet())System.arraycopy(e.getValue(),0,out,e.getKey()*PAYLOAD,e.getValue().length);
            delivered=p.frame;frames.headMap(delivered,true).clear();return out;
        }
        int[] missing(long id,long now,int retentionMs){
            Frame f=frames.get(id);
            if(f==null || now-f.began<3 || now-f.began>=retentionMs)return new int[0];
            int[] out=new int[Math.min(128,f.count)];
            int n=0;
            for(int start=0;start<f.count && n<out.length;start+=8){
                boolean parity=f.parity.containsKey(start/8);
                if(!parity && now-f.began<Math.min(15,retentionMs/3) && id==frames.lastKey())continue;
                for(int i=start;i<Math.min(start+8,f.count) && n<out.length;i++){
                    if(f.data.containsKey(i))continue;
                    if(parity){parity=false;continue;}
                    out[n++]=i;
                }
            }
            return Arrays.copyOf(out,n);
        }
    }
}
