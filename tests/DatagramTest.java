package org.lantern.client;
import java.nio.file.*;
import java.util.*;
public final class DatagramTest {
    static void check(boolean b,String s){if(!b)throw new AssertionError(s);}
    public static void main(String[] args)throws Exception{
        byte[] key=new byte[32],session=new byte[16];for(int i=0;i<32;i++)key[i]=(byte)i;for(int i=0;i<16;i++)session[i]=(byte)(32+i);
        DatagramCrypto sender=new DatagramCrypto(key,session,1),receiver=new DatagramCrypto(key,session,1);
        byte[] packet=sender.seal(new DatagramCrypto.Packet(1,7,0,1,3,new byte[]{1,2,3}));
        String expected=Files.readString(Path.of(args[0])).trim();check(HexFormat.of().formatHex(packet).equals(expected),"Java/CNG AES-GCM byte-identical wire vector");
        check(Arrays.equals(receiver.open(packet,packet.length).payload,new byte[]{1,2,3}),"AES-GCM decrypt");
        check(receiver.open(packet,packet.length)==null,"replay");
        for(int i=0;i<packet.length;i++){byte[] bad=packet.clone();bad[i]^=(byte)128;check(new DatagramCrypto(key,session,1).open(bad,bad.length)==null,"authentication covers every byte");}
        DatagramCrypto.Frames frames=new DatagramCrypto.Frames();byte[] data=new byte[1024];Arrays.fill(data,(byte)19);
        byte[] output=frames.accept(new DatagramCrypto.Packet(2,1,0,1,1024,data),1);
        check(Arrays.equals(output,data),"one-fragment FEC repair");
        check(frames.accept(new DatagramCrypto.Packet(1,2,0,16385,16*1024*1024+1,data),2)==null,"reassembly memory bound");
        System.out.println("Java datagram/CNG compatibility/authentication/replay/FEC checks passed");
    }
}
