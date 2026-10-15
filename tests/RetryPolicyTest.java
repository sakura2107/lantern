package org.lantern.client;
import java.io.*;
import java.net.*;
import javax.net.ssl.*;
public final class RetryPolicyTest {
    static int checks;
    static void check(boolean value){checks++;if(!value)throw new AssertionError("retry policy "+checks);}
    public static void main(String[] args){
        check(RetryPolicy.transientFailure(new ConnectException()));check(RetryPolicy.transientFailure(new SocketTimeoutException()));
        check(RetryPolicy.transientFailure(new Remote.TransportError(new EOFException())));
        check(!RetryPolicy.transientFailure(new IOException("disk full")));check(!RetryPolicy.transientFailure(new IOException("bad protocol frame")));
        check(!RetryPolicy.transientFailure(new EOFException("complete frame has truncated metadata")));
        for(int code:new int[]{1,2,3,4,5,6,7,9,10})check(!RetryPolicy.transientFailure(new Remote.RemoteError(code,"terminal")));
        check(RetryPolicy.transientFailure(new Remote.RemoteError(8,"busy")));
        check(!RetryPolicy.transientFailure(new Remote.TransportError(new SSLHandshakeException("pin changed"))));
        check(!RetryPolicy.transientFailure(new SecurityException()));
        check(RetryPolicy.delayMillis(1)==500);check(RetryPolicy.delayMillis(2)==1000);check(RetryPolicy.delayMillis(10)==30000);check(RetryPolicy.delayMillis(Integer.MAX_VALUE)==30000);
        System.out.println("retry policy: "+checks+" checks passed");
    }
}
