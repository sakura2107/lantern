package org.lantern.client;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Choreographer;

/**
 * The refresh cadence this app is actually given, which can differ from the panel's nominal rate:
 * ColorOS drops an untouched app to 60 Hz on a 120 Hz panel and returns it to 120 Hz on touch.
 * Runs only while a picture is decoding (one frame callback per refresh on its own thread).
 */
final class VsyncClock implements Choreographer.FrameCallback {
    private final HandlerThread thread = new HandlerThread("Lantern vsync");
    private Handler handler;
    private Choreographer choreographer;
    private volatile boolean running;
    private volatile long lastVsync, period = 16_666_667L;
    private long previous;

    synchronized void start() {
        if (running) return;
        running = true;
        if (handler == null) { thread.start(); handler = new Handler(thread.getLooper()); }
        handler.post(() -> {
            if (choreographer == null) choreographer = Choreographer.getInstance();
            previous = 0;
            choreographer.postFrameCallback(this);
        });
    }
    void stop() { running = false; }
    void quit() { running = false; thread.quitSafely(); }

    @Override public void doFrame(long frameTimeNanos) {
        if (previous != 0) {
            long delta = frameTimeNanos - previous;
            // Ignore missed callbacks; follow cadence changes within a few frames.
            if (delta > 3_000_000L && delta < 40_000_000L) period = (period * 3 + delta) / 4;
        }
        previous = frameTimeNanos;
        lastVsync = frameTimeNanos;
        if (running) choreographer.postFrameCallback(this);
    }
    long period() { return period; }
    long lastVsync() { return lastVsync; }
}
