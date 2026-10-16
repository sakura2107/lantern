package org.lantern.client;
import java.io.*;
import java.net.*;
import java.security.*;
import java.security.cert.*;
import javax.net.ssl.*;

/** Retry transport loss only. File, identity, integrity and protocol failures require a decision. */
final class RetryPolicy {
    /** Drop the stale stream and request a new IDR; this does not mean hardware is unsupported. */
    static final class StreamBacklog extends IOException {
        StreamBacklog() { super("桌面数据积压超过 300 ms"); }
    }
    static final int MAX_ATTEMPTS=10;
    static boolean transientFailure(Throwable error){
        boolean transport=false;
        for(int depth=0;error!=null&&depth<32;depth++,error=error.getCause()){
            if(error instanceof SSLException||error instanceof GeneralSecurityException||error instanceof SecurityException)return false;
            if(error instanceof Remote.RemoteError)return ((Remote.RemoteError)error).code==8;
            if(error instanceof StreamBacklog||error instanceof Remote.TransportError||error instanceof SocketException||error instanceof SocketTimeoutException||error instanceof UnknownHostException)transport=true;
        }
        return transport;
    }
    static long delayMillis(int attempts){return Math.min(30_000L,500L<<Math.min(6,Math.max(0,attempts-1)));}
}
