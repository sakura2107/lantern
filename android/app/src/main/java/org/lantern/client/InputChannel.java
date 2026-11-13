package org.lantern.client;
import java.io.*;
import java.util.*;
/** Worker-owned reliable control. No Activity references, UI mutation, or implicit control shutdown. */
final class InputChannel implements AutoCloseable {
    enum Phase { NORMAL, RECOVERING, DISCONNECTED }
    static final class State {
        final Phase phase; final long recoveringMillis;
        /** Not a network outage: revoked trust, ended session or an uncertain replay. Needs a deliberate restart. */
        final boolean terminal;
        State(Phase p,long n){this(p,n,false);}
        State(Phase p,long n,boolean t){phase=p;recoveringMillis=n;terminal=t;}
    }
    interface Events { void state(State s); default void result(long seq,int code,String reason){} default void result(long seq,int code,String reason,byte[] payload){result(seq,code,reason);}
        /** tag = the object given to submit (null for none). Coalesced moves never report. */
        default void result(long seq,int code,String reason,byte[] payload,Object tag){result(seq,code,reason,payload);} }
    static final class Command {
        final int op; final byte[] payload; final boolean move;
        boolean internal; Object tag;
        Command(int op,byte[] bytes) throws IOException {
            if(bytes.length>8192)throw new IOException("Input payload bound");
            this.op=op;payload=bytes.clone();
            DataInputStream r=Remote.reader(bytes);
            move=(op==22&&r.readInt()==0)||(op==36&&r.readInt()==2);
        }
    }
    static Command heartbeat(){try{Command c=new Command(40,new byte[0]);c.internal=true;return c;}catch(IOException impossible){throw new AssertionError(impossible);}}
    final Remote.Credentials credentials; final Events events;
    final ArrayDeque<Command> queue=new ArrayDeque<>();
    final String id=UUID.randomUUID().toString().replace("-","")+UUID.randomUUID().toString().replace("-","");
    final Thread worker;
    volatile Remote active; volatile java.net.Socket opening;
    volatile boolean closed; boolean terminal,sending; long acknowledged,recovery;
    InputChannel(Remote.Credentials c,Events e) {
        credentials=new Remote.Credentials(c.host,c.port,c.pin,c.token,c.endpoints);events=e;
        worker=new Thread(this::run,"Lantern reliable input");worker.start();
    }
    /** False = bounded backpressure: caller retains this event and retries; no critical event is evicted. */
    synchronized boolean submit(int op,byte[] payload) throws IOException {return submit(op,payload,null);}
    synchronized boolean submit(int op,byte[] payload,Object tag) throws IOException {
        Command c=new Command(op,payload);c.tag=tag;
        if(closed||terminal)return false;
        Command tail=queue.peekLast();
        if(c.move&&tail!=null&&tail.move&&tail.op==op&&(!sending||queue.size()>1))queue.removeLast();
        else if(queue.size()>=512)return false;
        queue.addLast(c);notifyAll();return true;
    }
    synchronized State state() {
        long elapsed=recovery==0?0:Math.max(1,(System.nanoTime()-recovery)/1_000_000);
        return new State(closed||terminal||elapsed>=30000?Phase.DISCONNECTED:elapsed>0?Phase.RECOVERING:Phase.NORMAL,elapsed,closed||terminal);
    }
    synchronized int pending(){return queue.size();}
    synchronized long acknowledged(){return acknowledged;}
    void publish(){if(events!=null)try{events.state(state());}catch(RuntimeException ignored){}}
    void run(){
        Remote remote=null;boolean replay=false;
        try {
            while(true){
                Command command;long seq;
                synchronized(this){
                    while(!closed&&terminal)wait();
                    if(!closed&&queue.isEmpty()&&remote!=null)wait(10000);
                    if(closed)return;
                    if(queue.isEmpty()&&remote!=null)queue.addLast(heartbeat());
                    command=queue.peekFirst();seq=acknowledged+1;sending=true;
                }
                boolean sent=false;
                try {
                    if(remote==null){
                        remote=new Remote(credentials,8,0,s->{opening=s;if(closed)s.close();});active=remote;
                        String caps=Remote.str(Remote.reader(remote.call(30,new byte[0])));
                        if(caps.contains(";session=end-v1"))Remote.sessionEndPin=credentials.pin;
                        replay=caps.contains(";input-replay=v1");
                    }
                    if(command==null){synchronized(this){sending=false;recovery=0;}publish();continue;}
                    sent=true;int code=0;String message="";byte[] payload;
                    if(replay){
                        DataInputStream ack=Remote.reader(remote.call(57,Remote.body(o->{o.writeInt(1);Remote.str(o,id);o.writeLong(seq);o.writeShort(command.op);o.writeInt(command.payload.length);o.write(command.payload);} )));
                        if(ack.readLong()!=seq)throw new Remote.RemoteError(4,"Input acknowledgement sequence");
                        code=ack.readInt();message=Remote.str(ack);payload=Remote.blob(ack,8192);
                        if(ack.available()!=0||code<0||code>10)throw new Remote.RemoteError(4,"Input result bound");
                    }else try{payload=remote.call(command.op,command.payload);}catch(Remote.RemoteError rejected){
                        if(rejected.code==2||Remote.endedByHost(rejected))throw rejected;code=rejected.code;message=rejected.getMessage();payload=new byte[0];}
                    synchronized(this){queue.removeFirst();acknowledged=seq;sending=false;recovery=0;}
                    if(events!=null&&!command.internal)try{events.result(seq,code,message,payload,command.tag);}catch(RuntimeException ignored){}
                    publish();
                }catch(Exception error){
                    if(remote!=null)try{remote.abort();}catch(IOException ignored){}
                    remote=null;active=null;
                    synchronized(this){
                        sending=false;
                        terminal=(sent&&!replay)||Remote.endedByHost(error)||error instanceof javax.net.ssl.SSLException||
                            error instanceof SecurityException||error instanceof Remote.RemoteError&&
                            (((Remote.RemoteError)error).code==2||((Remote.RemoteError)error).code==4);
                        if(recovery==0)recovery=System.nanoTime();
                    }
                    publish();
                    synchronized(this){if(!closed)wait(250);}
                }
            }
        }catch(InterruptedException stop){}finally{if(remote!=null)try{remote.abort();}catch(IOException ignored){}active=null;}
    }
    public void close(){
        synchronized(this){closed=true;notifyAll();}
        try{if(opening!=null)opening.close();}catch(IOException ignored){}
        try{if(active!=null)active.abort();}catch(IOException ignored){}
        worker.interrupt();
    }
}
