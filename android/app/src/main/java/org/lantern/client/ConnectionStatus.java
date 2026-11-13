package org.lantern.client;
/** Monotonic connection state: caller decides when/how to display it. No UI dependencies. */
final class ConnectionStatus {
    enum Phase { NORMAL, RECOVERING, DISCONNECTED }
    static final class Snapshot {
        final Phase phase;final long recoveringMillis;
        Snapshot(Phase phase,long elapsed){this.phase=phase;recoveringMillis=elapsed;}
    }
    long since;boolean terminal;
    synchronized void normal(){since=0;terminal=false;}
    synchronized void failure(boolean fatal){terminal=fatal;if(since==0)since=System.nanoTime();}
    synchronized Snapshot snapshot(){long elapsed=since==0?0:Math.max(1,(System.nanoTime()-since)/1_000_000);
        return new Snapshot(terminal||elapsed>=30000?Phase.DISCONNECTED:elapsed>0?Phase.RECOVERING:Phase.NORMAL,elapsed);}
}
