package org.lantern.client;

import android.app.Activity;
import android.content.*;
import android.os.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** A bounded independent control connection; native clipboard access stays on the foreground UI. */
final class DesktopTools implements AutoCloseable {
    interface Events {void status(String value);void monitors(List<Monitor> values);}
    static final class Monitor {final int index,x,y,width,height;final String name;Monitor(int i,int a,int b,int w,int h,String n){index=i;x=a;y=b;width=w;height=h;name=n;}}
    static final class Text {final int sequence;final boolean present;final String value;Text(int s,boolean p,String v){sequence=s;present=p;value=v;}}
    final Activity activity;final Events events;final Handler main=new Handler(Looper.getMainLooper());
    final ThreadPoolExecutor worker=new ThreadPoolExecutor(1,1,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(4));
    final AtomicLong epoch=new AtomicLong(),clipboardGeneration=new AtomicLong();final ClipboardManager clipboard;
    final ClipboardManager.OnPrimaryClipChangedListener changed=()->clipboardGeneration.incrementAndGet();
    volatile Remote active;volatile java.net.Socket opening;volatile boolean closed;
    DesktopTools(Activity a,Events e){activity=a;events=e;clipboard=(ClipboardManager)a.getSystemService(Context.CLIPBOARD_SERVICE);clipboard.addPrimaryClipChangedListener(changed);}
    static Text read(Remote r)throws Exception{DataInputStream in=Remote.reader(r.call(53,Remote.body(o->o.writeInt(1))));if(in.readInt()!=1)throw new IOException("剪贴板API版本不兼容");int seq=in.readInt(),present=in.readInt();String value=Remote.str(in);validate(value);if((present!=0&&present!=1)||in.available()!=0||(present==0&&!value.isEmpty()))throw new IOException("剪贴板响应无效");return new Text(seq,present==1,value);}
    static void validate(String s)throws IOException{if(s==null||s.indexOf(0)>=0||s.getBytes(StandardCharsets.UTF_8).length>65536)throw new IOException("仅支持最多64KiB UTF-8文本");}
    static void write(Remote r,int sequence,String value)throws Exception{validate(value);DataInputStream in=Remote.reader(r.call(54,Remote.body(o->{o.writeInt(1);o.writeInt(sequence);Remote.str(o,value);})));if(in.readInt()!=1)throw new IOException("剪贴板API版本不兼容");in.readInt();if(in.available()!=0)throw new IOException("剪贴板响应多余字段");}
    boolean current(long e){return !closed&&epoch.get()==e;}
    interface Action{void run(Remote r,long e)throws Exception;}
    void execute(Remote.Credentials c,int scopes,Action action){if(c==null){events.status("请先连接电脑");return;}long e=epoch.get();try{worker.execute(()->{if(!current(e))return;try(Remote r=new Remote(c,scopes,0,s->{opening=s;if(!current(e))s.close();})){active=r;if(current(e))action.run(r,e);}catch(Exception error){main.post(()->{if(current(e))events.status("桌面工具失败："+error.getMessage());});}finally{active=null;opening=null;}});}catch(RejectedExecutionException full){events.status("桌面工具队列已满，请稍后重试");}}
    void push(Remote.Credentials c){if(!activity.hasWindowFocus()){main.postDelayed(()->{if(!closed&&activity.hasWindowFocus())push(c);},150);return;}ClipData clip=clipboard.getPrimaryClip();if(clip==null||clip.getItemCount()!=1||clip.getItemAt(0).getText()==null){events.status("手机剪贴板不是单条文本，保留电脑内容");return;}String value=clip.getItemAt(0).getText().toString();long generation=clipboardGeneration.get();try{validate(value);}catch(IOException e){events.status(e.getMessage());return;}execute(c,8,(r,e)->{Text remote=read(r);if(!current(e)||generation!=clipboardGeneration.get())throw new IOException("手机剪贴板已变化，请重新复制");write(r,remote.sequence,value);main.post(()->{if(current(e))events.status("手机文本已复制到电脑；没有发送粘贴快捷键");});});}
    void pull(Remote.Credentials c){long generation=clipboardGeneration.get();execute(c,8,(r,e)->{Text remote=read(r);if(!remote.present)throw new IOException("电脑剪贴板不是文本，保留手机内容");main.post(()->{if(!current(e))return;if(!activity.hasWindowFocus()||generation!=clipboardGeneration.get()){events.status("手机剪贴板或前台状态已变化，保留原内容");return;}clipboard.setPrimaryClip(ClipData.newPlainText("Lantern远程文本",remote.value));events.status("电脑文本已复制到手机，可在手机应用粘贴");});});}
    void queryMonitors(Remote.Credentials c){execute(c,4,(r,e)->{DataInputStream in=Remote.reader(r.call(20,new byte[0]));int count=in.readInt();if(count<1||count>32)throw new IOException("显示器数量无效");List<Monitor> values=new ArrayList<>();for(int i=0;i<count;i++){int x=in.readInt(),y=in.readInt(),w=in.readInt(),h=in.readInt();String name=Remote.str(in);if(w<1||h<1||w>32768||h>32768)throw new IOException("显示器尺寸无效");values.add(new Monitor(i,x,y,w,h,name));}if(in.available()!=0)throw new IOException("显示器响应多余字段");main.post(()->{if(current(e))events.monitors(values);});});}
    void shortcut(Remote.Credentials c,int modifiers,int key){execute(c,8,(r,e)->{r.call(51,Remote.body(o->{o.writeInt(modifiers);o.writeInt(key);}));main.post(()->{if(current(e))events.status("快捷键已发送");});});}
    void cancel(){epoch.incrementAndGet();main.removeCallbacksAndMessages(null);worker.getQueue().clear();java.net.Socket s=opening;if(s!=null)try{s.close();}catch(IOException ignored){}Remote r=active;if(r!=null)try{r.abort();}catch(IOException ignored){}}
    public void close(){closed=true;cancel();clipboard.removePrimaryClipChangedListener(changed);main.removeCallbacksAndMessages(null);worker.shutdownNow();}
}
