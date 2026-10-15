package org.lantern.client;
import android.content.Context;
import android.net.*;
import android.os.*;

/** Default-link lifecycle only. Consumers own cancellation, authentication and retry budgets. */
final class NetworkWatcher implements AutoCloseable {
    final ConnectivityManager manager;final Handler main=new Handler(Looper.getMainLooper());final Runnable changed;
    Network previous;boolean closed,registered;
    NetworkWatcher(Context context,Runnable event){manager=context.getSystemService(ConnectivityManager.class);changed=event;manager.registerDefaultNetworkCallback(callback);registered=true;}
    void dispatch(){main.post(()->{if(!closed)changed.run();});}
    final ConnectivityManager.NetworkCallback callback=new ConnectivityManager.NetworkCallback(){
        public void onAvailable(Network network){if(previous!=null&&!previous.equals(network))dispatch();previous=network;}
        public void onLost(Network network){if(network.equals(previous))dispatch();}
    };
    public void close(){closed=true;if(registered){manager.unregisterNetworkCallback(callback);registered=false;}main.removeCallbacksAndMessages(null);}
}
