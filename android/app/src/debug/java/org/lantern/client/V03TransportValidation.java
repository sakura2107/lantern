package org.lantern.client;
import java.util.concurrent.atomic.AtomicInteger;
/** Backend-only loss injection; changes no system firewall/network settings. */
final class V03TransportValidation {
    static void run(DeviceTestRunner t,Remote.Credentials c)throws Exception{
        AtomicInteger recoveries=new AtomicInteger(),results=new AtomicInteger();
        try(InputChannel input=new InputChannel(c,new InputChannel.Events(){
            public void state(InputChannel.State s){if(s.phase==InputChannel.Phase.RECOVERING)recoveries.incrementAndGet();}
            public void result(long seq,int code,String reason,byte[] payload){results.incrementAndGet();}
        })){
            V03Validation.until(()->input.active!=null,5000,"idle input did not connect immediately");
            V03Validation.until(()->input.acknowledged()==1,12000,"real idle heartbeat missing");
            t.check(results.get()==0,"internal heartbeat is not a business result");
            input.active.abort();
            for(int i=0;i<64;i++)t.check(input.submit(40,new byte[0]),"focus flood accepted");
            V03Validation.until(()->input.acknowledged()==65,15000,"input did not replay after TLS interruption");
            t.check(recoveries.get()>0&&input.state().phase==InputChannel.Phase.NORMAL&&results.get()==64,"input recovery and ordered ACKs after idle loss");
        }
        try(Remote remote=new Remote(c,4,1)){
            DesktopSession s=new DesktopSession(remote,c,new AvcDesktop.Profile(1280,30,5000000));
            try{
                t.check(s.udp!=null,"phone UDP offer exists");
                // Unknown session IDs model a silently filtered path, with no unauthenticated replies.
                s.udp.outgoing.session[0]^=1;
                long start=System.nanoTime();s.ready();double seconds=(System.nanoTime()-start)/1e9;
                t.check(!s.usingUdp&&seconds>=1.4&&seconds<2.5,"phone blocked UDP falls back within probe budget");
                t.check(s.packet().length>0,"fallback desktop arrives on same TLS stream");
                t.report.put("blocked_udp_probe_seconds",seconds);
            }finally{s.close();}
        }
        try(Remote remote=new Remote(c,4,1)){
            DesktopSession s=new DesktopSession(remote,c,new AvcDesktop.Profile(1280,30,5000000));
            try{
                s.ready();t.check(s.usingUdp&&s.packet().length>0,"phone desktop UDP initially live");
                String id=s.id;s.udp.close();long start=System.nanoTime();byte[] packet=s.packet();
                t.check(!s.usingUdp&&packet.length>0&&s.id.equals(id),"midstream video UDP loss preserves TLS and stream ID");
                t.report.put("video_midstream_fallback_seconds",(System.nanoTime()-start)/1e9);
            }finally{s.close();}
        }
        try(Remote remote=new Remote(c,16,4);AudioSession s=new AudioSession(remote,true)){
            s.udp.outgoing.session[0]^=1;long start=System.nanoTime();s.ready();
            t.check(!s.usingUdp&&s.packet().length>0,"blocked audio UDP falls back to authenticated TCP");
            t.report.put("audio_blocked_probe_seconds",(System.nanoTime()-start)/1e9);
        }
        try(Remote remote=new Remote(c,16,4);AudioSession s=new AudioSession(remote,true)){
            s.ready();t.check(s.usingUdp&&s.packet().length>0,"phone audio UDP initially live");
            s.udp.close();long start=System.nanoTime();byte[] packet=s.packet();
            t.check(!s.usingUdp&&packet.length>0,"midstream audio UDP loss preserves TLS");
            t.report.put("audio_midstream_fallback_seconds",(System.nanoTime()-start)/1e9);
        }
        t.report.put("loss_injection","owned UDP socket close / unknown session; no firewall changes");
    }
}
