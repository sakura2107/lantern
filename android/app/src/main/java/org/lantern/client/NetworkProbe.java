package org.lantern.client;
import java.io.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Identity-only authenticated round-trip measurement; does not estimate available bandwidth. */
final class NetworkProbe {
    static final class Result {
        final double handshakeMillis,minMillis,medianMillis,maxMillis;
        Result(double connect,double[] sorted){handshakeMillis=connect;minMillis=sorted[0];medianMillis=sorted[sorted.length/2];maxMillis=sorted[sorted.length-1];}
    }
    static Result measure(Remote.Credentials c,AtomicBoolean cancel)throws Exception{
        long start=System.nanoTime();
        try(Remote r=new Remote(c,0,0)){
            double connect=(System.nanoTime()-start)/1e6;double[] samples=new double[5];
            for(int i=0;i<samples.length;i++){
                if(cancel.get())throw new IOException("诊断已取消");
                long nonce=System.nanoTime();DataInputStream reply=Remote.reader(r.call(50,Remote.body(o->o.writeLong(nonce))));
                if(reply.readLong()!=nonce||reply.available()!=0)throw new IOException("网络探测响应不匹配");
                samples[i]=(System.nanoTime()-nonce)/1e6;TransferPacer.SHARED.sample(samples[i]);
                if(i+1<samples.length)Thread.sleep(100);
            }
            Arrays.sort(samples);return new Result(connect,samples);
        }
    }
}
