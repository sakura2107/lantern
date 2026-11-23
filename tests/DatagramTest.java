package org.lantern.client;
import java.net.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
public final class DatagramTest {
    static void check(boolean b,String s){if(!b)throw new AssertionError(s);}
    static List<DatagramCrypto.Packet> fragment(long frame,byte[] content){
        int count=(content.length+DatagramCrypto.PAYLOAD-1)/DatagramCrypto.PAYLOAD;
        List<DatagramCrypto.Packet> parts=new ArrayList<>();
        byte[] parity=null;
        for(int i=0;i<count;i++){
            int off=i*DatagramCrypto.PAYLOAD,len=Math.min(DatagramCrypto.PAYLOAD,content.length-off);
            if(i%8==0)parity=new byte[len];
            byte[] part=Arrays.copyOfRange(content,off,off+len);
            for(int j=0;j<len;j++)parity[j]^=part[j];
            parts.add(new DatagramCrypto.Packet(DatagramCrypto.DATA,frame,i,count,content.length,part));
            if(i%8==7||i+1==count)parts.add(new DatagramCrypto.Packet(DatagramCrypto.PARITY,frame,i/8,count,content.length,parity.clone()));
        }
        return parts;
    }
    public static void main(String[] args)throws Exception{
        byte[] key=new byte[32],session=new byte[16];for(int i=0;i<32;i++)key[i]=(byte)i;for(int i=0;i<16;i++)session[i]=(byte)(32+i);
        DatagramCrypto sender=new DatagramCrypto(key,session,1),receiver=new DatagramCrypto(key,session,1);
        byte[] packet=sender.seal(new DatagramCrypto.Packet(DatagramCrypto.DATA,7,0,1,3,new byte[]{1,2,3}));
        byte[] emptyPkt=sender.seal(new DatagramCrypto.Packet(DatagramCrypto.FEEDBACK,7,0,0,0,new byte[0]));
        byte[] fullPayload=new byte[DatagramCrypto.PAYLOAD];for(int i=0;i<fullPayload.length;i++)fullPayload[i]=(byte)((i*37+11)&255);
        byte[] fullPkt=sender.seal(new DatagramCrypto.Packet(DatagramCrypto.DATA,8,0,2,DatagramCrypto.PAYLOAD+17,fullPayload));
        byte[] shortPayload=new byte[17];for(int i=0;i<shortPayload.length;i++)shortPayload[i]=(byte)(200+i);
        byte[] shortPkt=sender.seal(new DatagramCrypto.Packet(DatagramCrypto.DATA,8,1,2,DatagramCrypto.PAYLOAD+17,shortPayload));
        List<String> expected=Files.readAllLines(Path.of(args[0])).stream().map(String::trim).filter(s->!s.isEmpty()).toList();
        check(expected.size()>4,"missing C++ FEC vectors");
        int row=4;
        DatagramCrypto fromHost=new DatagramCrypto(key,session,1),toHost=new DatagramCrypto(key,session,2);
        DatagramCrypto.Frames cross=new DatagramCrypto.Frames();
        long id=10;
        Path reverse=Path.of(args[0]+".java.bin");
        try(DataOutputStream out=new DataOutputStream(Files.newOutputStream(reverse))){
            for(int size:new int[]{120,8192+17,8192,9216+37,152}){
                byte[] data=new byte[size],recovered=null;
                for(int i=0;i<size;++i)data[i]=(byte)(i*31+(i>>7)+size);
                for(DatagramCrypto.Packet part:fragment(id,data)){
                    byte[] wire=HexFormat.of().parseHex(expected.get(row++));
                    check(Arrays.equals(wire,sender.seal(part)),"CNG/Java FEC wire mismatch");
                    DatagramCrypto.Packet decoded=fromHost.open(wire,wire.length);
                    check(decoded!=null && Arrays.equals(decoded.payload,part.payload),"Java opens CNG FEC");
                    byte[] back=toHost.seal(part);out.writeShort(back.length);out.write(back);
                    if(decoded.type==DatagramCrypto.DATA && (decoded.index%8==7 || decoded.index+1==decoded.count))continue;
                    byte[] completed=cross.accept(decoded,1);if(completed!=null)recovered=completed;
                }
                check(Arrays.equals(recovered,data),"Java restores CNG frame "+size);
                ++id;
            }
        }
        check(row==expected.size(),"trailing C++ vectors");
        System.out.println("CNG -> Java: "+(row-4)+" byte-identical packets, 5 recovered frames");
        if(args.length>1)check(new ProcessBuilder(args[1],"--verify-java",reverse.toString()).inheritIO().start().waitFor()==0,
                              "CNG failed to recover Java vectors");
        check(HexFormat.of().formatHex(packet).equals(expected.get(0)),"Java/CNG short wire vector byte-identical");
        check(HexFormat.of().formatHex(emptyPkt).equals(expected.get(1)),"Java/CNG empty wire vector byte-identical");
        check(HexFormat.of().formatHex(fullPkt).equals(expected.get(2)),"Java/CNG full 1024B wire vector byte-identical");
        check(HexFormat.of().formatHex(shortPkt).equals(expected.get(3)),"Java/CNG trailing short wire vector byte-identical");
        check(Arrays.equals(receiver.open(packet,packet.length).payload,new byte[]{1,2,3}),"AES-GCM decrypt");
        check(receiver.open(packet,packet.length)==null,"replay");
        DatagramCrypto.Packet openedEmpty=receiver.open(emptyPkt,emptyPkt.length);
        check(openedEmpty!=null&&openedEmpty.payload.length==0&&openedEmpty.sequence==2,"empty payload decrypt");
        // Failed authentication must not mutate highest or seen replay state.
        long highestBefore=receiver.highest;long[] seenBefore=receiver.seen.clone();
        byte[] forgedHigh=fullPkt.clone();forgedHigh[24]=(byte)255;
        check(receiver.open(forgedHigh,forgedHigh.length)==null,"forged high sequence rejected");
        byte[] badTag=fullPkt.clone();badTag[badTag.length-1]^=1;
        check(receiver.open(badTag,badTag.length)==null,"mutated tag rejected");
        byte[] badBody=fullPkt.clone();badBody[DatagramCrypto.HEADER]^=1;
        check(receiver.open(badBody,badBody.length)==null,"mutated payload rejected");
        check(receiver.highest==highestBefore&&Arrays.equals(receiver.seen,seenBefore),"auth failures leave replay state unchanged");
        // Out-of-order short then full still accepted after auth failures.
        DatagramCrypto.Packet openedShort=receiver.open(shortPkt,shortPkt.length);
        check(openedShort!=null&&Arrays.equals(openedShort.payload,shortPayload)&&openedShort.sequence==4,"short packet after auth failure");
        DatagramCrypto.Packet openedFull=receiver.open(fullPkt,fullPkt.length);
        check(openedFull!=null&&Arrays.equals(openedFull.payload,fullPayload)&&openedFull.sequence==3,"full packet accepted out of order");
        check(receiver.open(fullPkt,fullPkt.length)==null,"full packet replay rejected");
        check(receiver.open(shortPkt,shortPkt.length)==null,"short packet replay rejected");
        for(int i=0;i<packet.length;i++){byte[] bad=packet.clone();bad[i]^=(byte)128;check(new DatagramCrypto(key,session,1).open(bad,bad.length)==null,"authentication covers every byte");}
        // Verify reused DatagramPacket wrapper handles short -> full -> short without truncation.
        try(DatagramSocket rx=new DatagramSocket(0,InetAddress.getLoopbackAddress());
            DatagramSocket tx=new DatagramSocket()){
            rx.setSoTimeout(1000);
            int port=rx.getLocalPort();
            InetAddress loopback=InetAddress.getLoopbackAddress();
            byte[] buf=new byte[1200];
            DatagramPacket reusable=new DatagramPacket(buf,buf.length);
            DatagramCrypto loopTx=new DatagramCrypto(key,session,1),loopRx=new DatagramCrypto(key,session,1);
            byte[][] wire={
                loopTx.seal(new DatagramCrypto.Packet(DatagramCrypto.DATA,1,0,1,3,new byte[]{1,2,3})),
                loopTx.seal(new DatagramCrypto.Packet(DatagramCrypto.DATA,2,0,1,fullPayload.length,fullPayload)),
                loopTx.seal(new DatagramCrypto.Packet(DatagramCrypto.DATA,3,0,1,shortPayload.length,shortPayload))
            };
            byte[][] expectedPayloads={new byte[]{1,2,3},fullPayload,shortPayload};
            for(int i=0;i<wire.length;i++){
                tx.send(new DatagramPacket(wire[i],wire[i].length,loopback,port));
                reusable.setLength(buf.length);
                rx.receive(reusable);
                DatagramCrypto.Packet got=loopRx.open(buf,reusable.getLength());
                check(got!=null&&Arrays.equals(got.payload,expectedPayloads[i]),"reused DatagramPacket alternating length "+i);
            }
        }
        DatagramCrypto.Frames frames=new DatagramCrypto.Frames();byte[] data=new byte[1024];Arrays.fill(data,(byte)19);
        byte[] output=frames.accept(new DatagramCrypto.Packet(2,1,0,1,1024,data),1);
        check(Arrays.equals(output,data),"one-fragment FEC repair");
        check(frames.accept(new DatagramCrypto.Packet(1,2,0,16385,16*1024*1024+1,data),2)==null,"reassembly memory bound");
        // Multi-group single-group FEC: 25,000 B (3 full groups + 1 partial tail group), parity-first + parity-last + duplicates.
        byte[] large=new byte[25000];for(int i=0;i<large.length;i++)large[i]=(byte)(i*79);
        List<DatagramCrypto.Packet> parts=fragment(10,large);
        DatagramCrypto.Frames multi=new DatagramCrypto.Frames();
        for(DatagramCrypto.Packet p:parts)if(p.type==DatagramCrypto.PARITY&&p.index==0){multi.accept(p,10);multi.accept(p,10);}
        byte[] repaired=null;
        for(DatagramCrypto.Packet p:parts){
            if(p.type==DatagramCrypto.DATA&&(p.index==5||p.index==11||p.index==24))continue;
            if(p.type==DatagramCrypto.PARITY&&p.index==0)continue;
            byte[] out=multi.accept(p,10);
            if(out!=null)repaired=out;
        }
        check(Arrays.equals(repaired,large),"single-group FEC repairs parity-first, parity-last and partial tail group");
        // Two losses in group 0 -> NACK leaves one to parity -> one retransmit + parity finishes frame.
        DatagramCrypto.Frames twoLoss=new DatagramCrypto.Frames();
        for(DatagramCrypto.Packet p:parts){if(p.type==DatagramCrypto.DATA&&(p.index==0||p.index==1))continue;twoLoss.accept(p,10);}
        check(Arrays.equals(twoLoss.missing(10,30,160),new int[]{1}),"NACK reserves one missing fragment for parity");
        byte[] recoveredTwo=null;
        for(DatagramCrypto.Packet p:parts)if(p.type==DatagramCrypto.DATA&&p.index==1)recoveredTwo=twoLoss.accept(p,31);
        check(Arrays.equals(recoveredTwo,large),"retransmission plus single-group parity repairs two losses");
        DatagramCrypto.Frames legacy=new DatagramCrypto.Frames();
        check(legacy.accept(new DatagramCrypto.Packet(2,99,0,1,120,new byte[121]),0)==null,"invalid parity length");
        check(Arrays.equals(legacy.accept(new DatagramCrypto.Packet(2,99,0,1,120,data),1),Arrays.copyOf(data,120)),
              "legacy padded parity restores exact short length");
        DatagramCrypto.Frames retained=new DatagramCrypto.Frames();
        for(long idLate:new long[]{20,21,19})retained.accept(new DatagramCrypto.Packet(1,idLate,0,2,1041,data),1);
        check(retained.missing(19,16,16).length==0 && retained.missing(20,16,16).length==1 &&
              retained.missing(20,17,16).length==0,"late frame and 120 fps retention");
        for(long late:new long[]{80,30})retained.accept(new DatagramCrypto.Packet(1,late,0,2,1041,data),230);
        check(retained.missing(30,245,160).length==0,"sequence gap cannot reopen an old NACK window");
        System.out.println("Java datagram/CNG compatibility/authentication/replay/FEC checks passed");
    }
}
