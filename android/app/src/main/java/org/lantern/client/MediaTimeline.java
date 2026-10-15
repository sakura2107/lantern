package org.lantern.client;
import java.util.Arrays;
/** AudioTrack timestamp master; bounded software observations, never acoustic/optical latency. */
final class MediaTimeline {
    private long owner,host,local;private boolean active,anchored;volatile boolean synchronize;
    private final long[] skew=new long[512];private long count;
    volatile long lastVideoHost,lastAudioHost,lastAudioLocal,lastDeltaUs,dropped,fallbacks;
    synchronized long beginAudio(){active=true;anchored=false;return ++owner;}
    synchronized void endAudio(long id){if(id==owner){active=false;anchored=false;}}
    synchronized void anchor(long id,long hostUs,long localNs){if(id==owner&&active&&hostUs>0&&localNs>0){host=hostUs;local=localNs;lastAudioHost=host;lastAudioLocal=local;anchored=true;}}
    synchronized long deadline(long hostUs,long now){
        if(!synchronize||!active||!anchored||now<local||now-local>500_000_000L)return now;
        long delta=hostUs-host;lastVideoHost=hostUs;lastDeltaUs=delta;if(delta>650_000||delta< -650_000){fallbacks++;return now;}
        long at=local+delta*1000;if(at>now+350_000_000L){fallbacks++;return now;}if(at<now-80_000_000L){dropped++;return -1;}
        return Math.max(at,now);
    }
    synchronized long videoDelayBudget(long now){return synchronize&&active&&anchored&&now>=local&&now-local<=500_000_000L?350_000_000L:0;}
    synchronized void observe(long hostUs,long now){if(active&&anchored&&now>=local&&now-local<=500_000_000L)skew[(int)(count++%skew.length)]=hostUs-host-(now-local)/1000;}
    synchronized long[] samples(){int n=(int)Math.min(count,skew.length);long[] result=new long[n];for(int i=0;i<n;i++)result[i]=skew[(int)((count-n+i)%skew.length)];return result;}
}
