package org.lantern.client;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Shared bulk gate: bounded bursts, one queue, RTT feedback and explicit media reservation. */
final class TransferPacer {
    static final TransferPacer SHARED = new TransferPacer();
    volatile long userLimit = 32L * 1024 * 1024;
    volatile boolean realtime, movie;
    volatile double baseRtt, latestRtt;
    long adaptive = 32L * 1024 * 1024, next;
    synchronized void sample(double milliseconds) {
        if (milliseconds <= 0 || !Double.isFinite(milliseconds))
            return;
        latestRtt = milliseconds;
        if (baseRtt == 0 || milliseconds < baseRtt)
            baseRtt = milliseconds;
        if (milliseconds > baseRtt + 15)
            adaptive = Math.max(256 * 1024, adaptive / 2);
        else
            adaptive = Math.min(32L * 1024 * 1024, adaptive + 256 * 1024);
    }
    synchronized long rate() {
        return Math.min(userLimit, Math.min(adaptive, movie      ? 2L * 1024 * 1024
                                                      : realtime ? 4L * 1024 * 1024
                                                                 : 32L * 1024 * 1024));
    }
    void acquire(int bytes, AtomicBoolean cancel) throws Exception {
        long due;
        synchronized (this) {
            long now = System.nanoTime();
            long cost = bytes * 1_000_000_000L / rate();
            next = Math.max(now, next) + cost;
            due = next;
        }
        while (System.nanoTime() < due) {
            if (cancel.get())
                throw new IOException("传输已暂停，进度保留");
            Thread.sleep(Math.min(20, Math.max(1, (due - System.nanoTime()) / 1_000_000)));
        }
    }
}
