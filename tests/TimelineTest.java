package org.lantern.client;
public final class TimelineTest {
 static int checks;
 static void check(boolean b,String s){if(!b)throw new AssertionError(s);checks++;}
 public static void main(String[] args)throws Exception{
  var t=new MediaTimeline();long now=2_000_000_000L,host=5_000_000;
  check(t.deadline(host,now)==now,"video only immediate");t.synchronize=true;long owner=t.beginAudio();
  check(t.deadline(host,now)==now,"warming audio no wait");t.anchor(owner,host,now);
  check(t.deadline(host+60_000,now)==now+60_000_000,"hardware audio master");
  check(t.deadline(host-90_000,now)==-1,"late drop");check(t.deadline(host+400_000,now)==now,"future bound");
  check(t.deadline(host,now+600_000_000)==now+600_000_000,"stale anchor no stall");
  long newer=t.beginAudio();t.endAudio(owner);t.anchor(owner,host,now);
  check(t.deadline(host+60_000,now)==now,"old owner cannot anchor");t.anchor(newer,host,now);
  t.endAudio(owner);check(t.deadline(host+60_000,now)==now+60_000_000,"old owner cannot stop new audio");
  for(int i=0;i<1000;i++)t.observe(host,now);check(t.samples().length==512,"bounded observations");
  t.endAudio(newer);check(t.deadline(host+60_000,now)==now,"stop removes video delay");
  for(String input:new String[]{"pc.local:47443","[::1]:47443","[fe80::1%3]:443","::1"}){var e=Endpoint.parse(input,47443);check(Endpoint.parse(e.toString(),47443).host.equals(e.host),"endpoint round trip");}
  for(String input:new String[]{"","a:0","a:65536","a:-1","[::1]:","[::1]junk","a/b","x@y","a b"}){boolean rejected=false;try{Endpoint.parse(input,47443);}catch(java.io.IOException e){rejected=true;}check(rejected,"endpoint invalid "+input);}
  boolean invalid=false;try{Remote.body(o->Remote.str(o,"\ud800"));}catch(java.io.IOException e){invalid=true;}check(invalid,"invalid Unicode rejected");
  String nonce="a".repeat(64),pin="b".repeat(64);check(Discovery.query(nonce).length==128,"discovery bounded query");
  byte[] reply=Remote.body(o->{o.writeBytes("LANTERN!");o.writeInt(1);Remote.str(o,nonce);Remote.str(o,pin);Remote.str(o,"fixture");o.writeInt(47443);});check(Discovery.parse(reply,nonce,"127.0.0.1").pin.equals(pin),"discovery reply");
  for(int i=0;i<reply.length;i++){boolean rejected=false;try{Discovery.parse(java.util.Arrays.copyOf(reply,i),nonce,"127.0.0.1");}catch(java.io.IOException e){rejected=true;}check(rejected,"discovery truncated");}
  System.out.println("Timeline/endpoint/discovery checks: "+checks);
 }
}
