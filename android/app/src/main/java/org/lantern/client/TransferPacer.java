package org.lantern.client;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Optional user-selected bulk limit. TCP controls congestion; RTT is telemetry only. */
final class TransferPacer {
    static final TransferPacer SHARED = new TransferPacer();
    // The existing UI stores 32 MiB/s for its "no additional limit" choice.
    // Keep that preferences value, but do not sleep or impose a ceiling for it.
    static final long UNLIMITED = 32L * 1024 * 1024;
    volatile long userLimit = UNLIMITED;
    // Retained for existing UI/provider callers; media activity never reduces the rate.
    volatile boolean realtime, movie;
    volatile double latestRtt;
    long next;
    /** Bytes file transfers actually moved, per direction: what the speed lines show instead of a cap. */
    static final class Meter {
        static final long SLOT = 500_000_000L; // ns
        static final int SLOTS = 6;
        final long[] slot = new long[SLOTS], count = new long[SLOTS];
        synchronized void add(long bytes) {
            long s = System.nanoTime() / SLOT;
            int i = (int)(s % SLOTS);
            if (slot[i] != s) { slot[i] = s; count[i] = 0; }
            count[i] += bytes;
        }
        /** Bytes per second over the last complete 2.5 s; the slot still filling is left out. */
        synchronized double rate() {
            long s = System.nanoTime() / SLOT, sum = 0;
            for (int i = 0; i < SLOTS; i++)
                if (slot[i] < s && slot[i] > s - SLOTS) sum += count[i];
            return sum * 1e9 / (SLOT * (SLOTS - 1));
        }
    }
    final Meter down = new Meter(), up = new Meter();
    boolean limited() { long l = userLimit; return l > 0 && l < UNLIMITED; }
    synchronized void sample(double milliseconds) {
        if (milliseconds <= 0 || !Double.isFinite(milliseconds))
            return;
        latestRtt = milliseconds;
    }
    synchronized long rate() {
        // Legacy numeric presentation API. UNLIMITED is a selection, not a measured rate.
        return userLimit > 0 ? userLimit : UNLIMITED;
    }
    void acquire(int bytes, AtomicBoolean cancel) throws Exception {
        if (cancel.get())
            throw new IOException("传输已暂停，进度保留");
        long due;
        synchronized (this) {
            if (userLimit <= 0 || userLimit >= UNLIMITED) {
                next = 0;
                return;
            }
            long now = System.nanoTime();
            long cost = bytes * 1_000_000_000L / rate();
            next = Math.max(now, next) + cost;
            due = next;
        }
        while (System.nanoTime() < due) {
            if (cancel.get())
                throw new IOException("传输已暂停，进度保留");
            if (userLimit <= 0 || userLimit >= UNLIMITED) {
                synchronized (this) { next = 0; }
                return;
            }
            Thread.sleep(Math.min(20, Math.max(1, (due - System.nanoTime()) / 1_000_000)));
        }
    }
}
