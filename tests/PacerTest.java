package org.lantern.client;
import java.util.concurrent.atomic.AtomicBoolean;
public final class PacerTest {
    static int checks;
    static void check(boolean value){checks++;if(!value)throw new AssertionError("pacer check "+checks);}
    public static void main(String[] args)throws Exception{
        TransferPacer p=new TransferPacer();p.sample(2);long normal=p.rate();p.sample(40);check(p.rate()==normal/2);
        p.realtime=true;check(p.rate()<=4*1024*1024);p.movie=true;check(p.rate()<=2*1024*1024);
        p.userLimit=1024*1024;check(p.rate()==1024*1024);long start=System.nanoTime();p.acquire(128*1024,new AtomicBoolean());long elapsed=System.nanoTime()-start;check(elapsed>=120_000_000&&elapsed<2_000_000_000L);
        boolean cancelled=false;try{p.acquire(128*1024,new AtomicBoolean(true));}catch(java.io.IOException e){cancelled=true;}check(cancelled);
        for(int i=0;i<100;i++)p.sample(100);check(p.rate()==256*1024);p.sample(Double.NaN);check(p.rate()==256*1024);
        p.userLimit=32L*1024*1024;p.movie=p.realtime=false;for(int i=0;i<200;i++)p.sample(2);check(p.rate()==32L*1024*1024);
        System.out.println("pacer: "+checks+" checks passed");
    }
}
