package org.lantern.client;

import android.app.*;
import android.content.*;
import android.content.pm.ServiceInfo;
import android.net.*;
import android.os.*;

/** One application-owned queue. Foreground lifetime exists only for user-started pending work. */
public final class TransferService extends Service {
    static final String CHANNEL="lantern-transfers",PAUSE="org.lantern.client.PAUSE_TRANSFERS";
    static final int NOTIFICATION=410;
    static volatile TransferService active;
    static TransferQueue shared;
    final Handler main=new Handler(Looper.getMainLooper());
    PowerManager.WakeLock wake;
    ConnectivityManager connectivity;
    Network previousNetwork;
    boolean watching;
    static synchronized TransferQueue acquire(MainActivity activity){
        if(shared==null||shared.closed)shared=new TransferQueue(activity.getApplicationContext());
        shared.attach(activity);return shared;
    }
    static synchronized void ensure(Context context,TransferQueue queue){
        if(active!=null&&shared==queue)return;
        shared=queue;
        context.startForegroundService(new Intent(context,TransferService.class));
    }
    static void shutdown(Context context){
        TransferQueue queue; synchronized(TransferService.class){queue=shared;}
        if(queue!=null)queue.close();
        context.stopService(new Intent(context,TransferService.class));
    }
    final ConnectivityManager.NetworkCallback networkCallback=new ConnectivityManager.NetworkCallback(){
        public void onAvailable(Network network){synchronized(this){if(previousNetwork!=null&&!previousNetwork.equals(network)){TransferQueue q=shared;if(q!=null)q.networkChanged();}previousNetwork=network;}}
        public void onLost(Network network){synchronized(this){if(network.equals(previousNetwork)){TransferQueue q=shared;if(q!=null)q.networkChanged();}}}
    };
    Notification notification(){
        Intent open=new Intent(this,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP).putExtra("open_transfers",true);
        PendingIntent content=PendingIntent.getActivity(this,0,open,PendingIntent.FLAG_IMMUTABLE|PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent pause=PendingIntent.getService(this,1,new Intent(this,TransferService.class).setAction(PAUSE),PendingIntent.FLAG_IMMUTABLE|PendingIntent.FLAG_UPDATE_CURRENT);
        TransferQueue q=shared;TransferQueue.Task task=q==null?null:q.running;
        String title=task==null?"Lantern 文件传输":(task.kind==0?"下载 · ":"上传 · ")+task.name;
        String detail=q==null?"准备传输":q.notificationText();
        Notification.Builder builder=new Notification.Builder(this,CHANNEL).setSmallIcon(android.R.drawable.stat_sys_download).setContentTitle(title).setContentText(detail).setContentIntent(content).setOngoing(true).setOnlyAlertOnce(true).setVisibility(Notification.VISIBILITY_PRIVATE).setCategory(Notification.CATEGORY_PROGRESS).addAction(new Notification.Action.Builder(null,"暂停全部",pause).build());
        if(task!=null){builder.setProgress(1000,task.total==0?0:(int)Math.min(1000,task.current*1000.0/task.total),task.total==0);}
        return builder.build();
    }
    @Override public void onCreate(){
        super.onCreate();active=this;
        getSystemService(NotificationManager.class).createNotificationChannel(new NotificationChannel(CHANNEL,"文件上传与下载",NotificationManager.IMPORTANCE_LOW));
        if(Build.VERSION.SDK_INT>=29)startForeground(NOTIFICATION,notification(),ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);else startForeground(NOTIFICATION,notification());
        PowerManager power=getSystemService(PowerManager.class);wake=power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"Lantern:transfer");wake.setReferenceCounted(false);
        connectivity=getSystemService(ConnectivityManager.class);
        try{connectivity.registerDefaultNetworkCallback(networkCallback);watching=true;}catch(SecurityException ignored){}
        main.post(tick);
    }
    final Runnable tick=new Runnable(){public void run(){
        TransferQueue q=shared;
        if(q==null||q.closed||!q.hasPending()){
            if(wake!=null&&wake.isHeld())wake.release();stopForeground(STOP_FOREGROUND_REMOVE);stopSelf();return;
        }
        if(q.running!=null){if(!wake.isHeld())wake.acquire(60_000);}else if(wake.isHeld())wake.release();
        getSystemService(NotificationManager.class).notify(NOTIFICATION,notification());main.postDelayed(this,1000);
    }};
    @Override public int onStartCommand(Intent intent,int flags,int id){
        if(intent!=null&&PAUSE.equals(intent.getAction())){TransferQueue q=shared;if(q!=null)q.pauseAll("已从通知暂停");}
        main.removeCallbacks(tick);main.post(tick);return START_NOT_STICKY;
    }
    // Android 15+ limits dataSync services; preserve all partial files rather than silently lose work.
    @Override public void onTimeout(int startId,int foregroundServiceType){TransferQueue q=shared;if(q!=null)q.pauseAll("系统后台传输时限已到，请回到应用继续");stopForeground(STOP_FOREGROUND_REMOVE);stopSelf();}
    @Override public void onDestroy(){main.removeCallbacks(tick);if(watching)connectivity.unregisterNetworkCallback(networkCallback);if(wake!=null&&wake.isHeld())wake.release();if(active==this)active=null;super.onDestroy();}
    @Override public IBinder onBind(Intent intent){return null;}
}
