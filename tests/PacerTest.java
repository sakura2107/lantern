package org.lantern.client;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

public final class PacerTest {
    static int checks;
    static void check(boolean value) {
        checks++;
        if (!value) throw new AssertionError("pacer check " + checks);
    }
    static void cancelled(TransferPacer p) throws Exception {
        boolean stopped = false;
        try { p.acquire(262144, new AtomicBoolean(true)); }
        catch (IOException expected) { stopped = true; }
        check(stopped);
    }
    public static void main(String[] args) throws Exception {
        TransferPacer p = new TransferPacer();
        long normal = p.rate();
        // Actual failure scenario: a LAN minimum survives a move to mobile IPv6.
        p.sample(2.850157);
        for (int i = 0; i < 100; i++) p.sample(60.280156);
        check(p.rate() == normal);
        // Real measured mobile RTT sequence must not silently reduce file throughput.
        for (double rtt : new double[]{47.54,69.13,54.78,56.70,43.36,107.44,53.96,
                65.85,63.40,136.78,59.63,117.41,122.05,80.09,56.57,60.43,131.47,79.02})
            p.sample(rtt);
        check(p.rate() == normal);
        double latest = p.latestRtt;
        for (double invalid : new double[]{0,-1,Double.NaN,Double.POSITIVE_INFINITY})
            p.sample(invalid);
        check(p.latestRtt == latest);
        p.realtime = true;
        check(p.rate() == normal);
        p.movie = true;
        check(p.rate() == normal);
        // The existing 32 MiB/s "no additional limit" selection must never sleep.
        long start = System.nanoTime();
        for (int i = 0; i < 512; i++) p.acquire(262144, new AtomicBoolean());
        check(System.nanoTime() - start < 1_000_000_000L);
        check(p.next == 0);
        cancelled(p);

        // Explicit user limits still apply even after telemetry and media state changes.
        p.userLimit = 1024 * 1024;
        for (int i = 0; i < 50; i++) p.sample(150);
        check(p.rate() == 1024 * 1024);
        start = System.nanoTime();
        p.acquire(128 * 1024, new AtomicBoolean());
        long elapsed = System.nanoTime() - start;
        check(elapsed >= 120_000_000L && elapsed < 2_000_000_000L);
        cancelled(p);

        // Removing a manual limit releases an in-flight wait and any queued time debt.
        p.next = System.nanoTime() + 5_000_000_000L;
        Thread unlimited = new Thread(() -> {
            try { Thread.sleep(50); } catch (InterruptedException e) { throw new AssertionError(e); }
            p.userLimit = TransferPacer.UNLIMITED;
        });
        unlimited.start();
        start = System.nanoTime();
        p.acquire(262144, new AtomicBoolean());
        unlimited.join();
        check(System.nanoTime() - start < 1_000_000_000L);
        check(p.next == 0);
        p.userLimit = 0;
        p.acquire(262144, new AtomicBoolean());
        check(p.rate() == TransferPacer.UNLIMITED && p.next == 0);
        System.out.println("pacer: " + checks + " checks passed");
    }
}
