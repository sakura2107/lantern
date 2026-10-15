package org.lantern.client;
import java.io.*;
import java.net.*;
import java.security.SecureRandom;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;
final class Discovery {
    static final int PORT=47442;
    static final class Device {final String host,label,pin;final int port;Device(String h,String l,String p,int number){host=h;label=l;pin=p;port=number;}String address(){return host+":"+port;}}
    static byte[] query(String nonce)throws IOException{if(!nonce.matches("[0-9a-f]{64}"))throw new IOException("Discovery nonce");ByteArrayOutputStream bytes=new ByteArrayOutputStream(128);DataOutputStream o=new DataOutputStream(bytes);o.writeBytes("LANTERN?");o.writeInt(1);Remote.str(o,nonce);return Arrays.copyOf(bytes.toByteArray(),128);}
    static Device parse(byte[] payload,String nonce,String source)throws IOException{
        if(payload.length>256||!nonce.matches("[0-9a-f]{64}"))throw new IOException("Discovery length");DataInputStream in=Remote.reader(payload);byte[] magic=new byte[8];in.readFully(magic);if(!Arrays.equals(magic,"LANTERN!".getBytes(java.nio.charset.StandardCharsets.US_ASCII))||in.readInt()!=1||!Remote.str(in).equals(nonce))throw new IOException("Discovery signature/nonce");String pin=Remote.str(in),label=Remote.str(in);int port=in.readInt();if(!pin.matches("[0-9a-f]{64}")||label.isEmpty()||label.getBytes(java.nio.charset.StandardCharsets.UTF_8).length>64||port<1||port>65535||in.available()!=0)throw new IOException("Discovery metadata");for(char c:label.toCharArray())if(c<32||c==127)throw new IOException("Discovery label");return new Device(source,label,pin,port);
    }
    static boolean local(InetAddress address){byte[] a=address.getAddress();if(a.length!=4)return false;int x=a[0]&255,y=a[1]&255;return x==10||x==127||x==192&&y==168||x==172&&y>=16&&y<=31||x==169&&y==254;}
    static List<Device> scan(AtomicBoolean cancel,String target)throws IOException{
        byte[] random=new byte[32];new SecureRandom().nextBytes(random);String nonce=Remote.hex(random);byte[] request=query(nonce);List<Device> devices=new ArrayList<>();
        try(DatagramSocket socket=new DatagramSocket()){socket.setBroadcast(true);socket.setSoTimeout(100);InetAddress destination=InetAddress.getByName(target==null?"255.255.255.255":target);long start=System.nanoTime(),next=start;
            while(System.nanoTime()-start<1_500_000_000L){if(cancel.get())throw new IOException("发现已取消");long now=System.nanoTime();if(now>=next){socket.send(new DatagramPacket(request,request.length,destination,PORT));next=now+500_000_000L;}DatagramPacket packet=new DatagramPacket(new byte[257],257);try{socket.receive(packet);}catch(SocketTimeoutException timeout){continue;}if(packet.getPort()!=PORT||packet.getLength()>256||!local(packet.getAddress()))continue;try{Device d=parse(Arrays.copyOf(packet.getData(),packet.getLength()),nonce,packet.getAddress().getHostAddress());boolean duplicate=false;for(Device old:devices)if(old.host.equals(d.host)&&old.port==d.port)duplicate=true;if(!duplicate&&devices.size()<32)devices.add(d);}catch(IOException invalid){}
            }
        }return devices;
    }
}
