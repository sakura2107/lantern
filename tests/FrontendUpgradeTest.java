package org.lantern.client;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

/** Unit tests for WP-PERF-02 quality UI deduplication/coalescing and two-finger wheel gesture boundaries. */
public final class FrontendUpgradeTest {
    static int checks;

    static void check(boolean cond, String name) {
        checks++;
        if (!cond) throw new AssertionError("Failed: " + name);
    }

    static DesktopSession.Status status(int w, int h, int fps, int br, String codec, String path,
                                        int selW, boolean constrained, boolean dropping) {
        return new DesktopSession.Status(w, h, fps, br, codec, path, selW, constrained, dropping);
    }

    static void testStatusEquality() {
        DesktopSession.Status base = status(1920, 1080, 120, 12_000_000, "hevc", "udp", 1920, false, false);
        DesktopSession.Status copy = status(1920, 1080, 120, 12_000_000, "hevc", "udp", 1920, false, false);
        check(base.sameAs(copy) && base.equals(copy) && base.hashCode() == copy.hashCode(), "identical 9-field status");
        check(!base.sameAs(null), "null status not equal");
        check(!base.sameAs(status(2560, 1080, 120, 12_000_000, "hevc", "udp", 1920, false, false)), "width diff");
        check(!base.sameAs(status(1920, 1440, 120, 12_000_000, "hevc", "udp", 1920, false, false)), "height diff");
        check(!base.sameAs(status(1920, 1080, 60, 12_000_000, "hevc", "udp", 1920, false, false)), "fps diff");
        check(!base.sameAs(status(1920, 1080, 120, 8_000_000, "hevc", "udp", 1920, false, false)), "bitrate diff");
        check(!base.sameAs(status(1920, 1080, 120, 12_000_000, "h264", "udp", 1920, false, false)), "codec diff");
        check(!base.sameAs(status(1920, 1080, 120, 12_000_000, "hevc", "tcp", 1920, false, false)), "path diff");
        check(!base.sameAs(status(1920, 1080, 120, 12_000_000, "hevc", "udp", 1280, false, false)), "selectedWidth diff");
        check(!base.sameAs(status(1920, 1080, 120, 12_000_000, "hevc", "udp", 1920, true, false)), "constrained diff");
        check(!base.sameAs(status(1920, 1080, 120, 12_000_000, "hevc", "udp", 1920, false, true)), "dropping diff");
    }

    static void testQualityPublisherDedupAndCoalesce() {
        ArrayDeque<Runnable> uiQueue = new ArrayDeque<>();
        List<DesktopSession.Status> delivered = new ArrayList<>();
        List<Long> deliveredEpochs = new ArrayList<>();
        AvcDesktop.QualityPublisher pub = new AvcDesktop.QualityPublisher(uiQueue::addLast, (epoch, ctx, st) -> {
            deliveredEpochs.add(epoch);
            delivered.add(st);
        });

        // 1. Steady-state 120fps for 10 seconds (1200 frames): only 1 post and 1 delivery.
        for (int i = 0; i < 1200; i++) {
            pub.publish(1L, "ctx1", status(1920, 1080, 120, 12_000_000, "hevc", "udp", 1920, false, false));
            while (!uiQueue.isEmpty()) uiQueue.removeFirst().run();
        }
        check(pub.postedCount == 1, "steady state posts exactly once: " + pub.postedCount);
        check(delivered.size() == 1, "steady state delivers once: " + delivered.size());

        // 2. Burst changes while UI thread has not drained yet: coalesced into 1 post & latest value.
        DesktopSession.Status s2 = status(1920, 1080, 60, 10_000_000, "hevc", "udp", 1920, false, false);
        DesktopSession.Status s3 = status(1280, 720, 60, 6_000_000, "hevc", "tcp", 1280, true, false);
        DesktopSession.Status s4 = status(2560, 1440, 120, 20_000_000, "hevc", "udp", 2560, false, false);
        pub.publish(1L, "ctx1", s2);
        pub.publish(1L, "ctx1", s3);
        pub.publish(1L, "ctx1", s4);
        check(pub.postedCount == 2 && uiQueue.size() == 1, "burst coalesces to one queued runnable");
        while (!uiQueue.isEmpty()) uiQueue.removeFirst().run();
        check(delivered.size() == 2 && delivered.get(1).sameAs(s4), "burst delivers latest snapshot only");

        // 3. New epoch with identical status MUST deliver on first frame.
        pub.publish(2L, "ctx2", status(2560, 1440, 120, 20_000_000, "hevc", "udp", 2560, false, false));
        check(pub.postedCount == 3 && uiQueue.size() == 1, "new epoch always delivers first frame");
        while (!uiQueue.isEmpty()) uiQueue.removeFirst().run();
        check(delivered.size() == 3 && deliveredEpochs.get(2) == 2L, "new epoch recorded");

        // 4. clearEpoch drops pending delivery for a torn-down epoch.
        pub.publish(2L, "ctx2", s3);
        pub.clearEpoch(2L);
        while (!uiQueue.isEmpty()) uiQueue.removeFirst().run();
        check(delivered.size() == 3, "cleared epoch drops stale pending update");

        // 5. Re-entrant update arriving while drain() is executing sink must not be lost.
        ArrayDeque<Runnable> reQueue = new ArrayDeque<>();
        List<DesktopSession.Status> reDelivered = new ArrayList<>();
        AvcDesktop.QualityPublisher[] holder = {null};
        DesktopSession.Status first = status(1920, 1080, 60, 12_000_000, "h264", "udp", 1920, false, false);
        DesktopSession.Status during = status(1920, 1080, 60, 12_000_000, "h264", "tcp", 1920, false, false);
        DesktopSession.Status after = status(1280, 720, 60, 8_000_000, "h264", "tcp", 1280, false, false);
        holder[0] = new AvcDesktop.QualityPublisher(reQueue::addLast, (epoch, ctx, st) -> {
            reDelivered.add(st);
            if (reDelivered.size() == 1) holder[0].publish(epoch, ctx, during);
        });
        holder[0].publish(1L, "ctx", first);
        check(reQueue.size() == 1, "initial re-entrant post queued");
        reQueue.removeFirst().run();
        check(reDelivered.size() == 2 && reDelivered.get(0).sameAs(first) && reDelivered.get(1).sameAs(during),
              "in-flight update delivered in same drain without loss");
        check(reQueue.isEmpty(), "no redundant post during active drain");
        holder[0].publish(1L, "ctx", after);
        check(reQueue.size() == 1, "post resumes cleanly after drain exits");
        reQueue.removeFirst().run();
        check(reDelivered.size() == 3 && reDelivered.get(2).sameAs(after), "subsequent update delivered");
    }

    static void testWheelGestureBoundaries() {
        float d = 3.0f; // xxhdpi
        DesktopView.WheelGesture g = new DesktopView.WheelGesture(d);

        // 1. Control disabled -> never enters wheel.
        g.begin(300, 500, 500, 500, 0, false, 1f);
        check(g.move(300, 600, 500, 600, 40, false, 1f) == 0 && !g.isWheel(), "control disabled blocks wheel");

        // 2. Zoomed in (> 1.02f) -> yields to viewport pan, never wheels.
        g.begin(300, 500, 500, 500, 0, true, 1.5f);
        check(g.move(300, 600, 500, 600, 40, true, 1.5f) == 0 && !g.isWheel(), "zoomed view yields to pan");

        // 3. Horizontal pinch-zoom (DeviceTestRunner.pinch trajectory) -> classified as PINCH, never wheels.
        g.begin(400, 500, 600, 500, 0, true, 1f);
        for (int step = 1; step <= 8; step++) {
            int delta = g.move(400 - step * 25, 500, 600 + step * 25, 500, step * 35L, true, 1f);
            check(delta == 0 && !g.isWheel(), "pinch step " + step + " must not wheel");
        }
        check(g.isPinch(), "horizontal spread locked as pinch");

        // 4. Two-finger tap (tiny jitter < 12dp) -> stays UNDECIDED for right-click tap.
        g.begin(400, 500, 520, 500, 0, true, 1f);
        check(g.move(404, 506, 523, 505, 60, true, 1f) == 0 && !g.isWheel() && !g.isPinch(), "two-finger tap stays undecided");

        // 5. Parallel two-finger downward swipe -> enters WHEEL, moves cursor once, emits +120 notches.
        g.begin(400, 500, 540, 500, 100, true, 1f);
        int d1 = g.move(400, 500 + 20 * d, 540, 500 + 20 * d, 140, true, 1f);
        check(g.isWheel(), "parallel vertical drag enters wheel");
        check(g.consumeEnteredWheel() && !g.consumeEnteredWheel(), "cursor move trigger fires once");
        check(g.startMidX == 470f && g.startMidY == 500f, "cursor target is initial two-finger midpoint");
        check(d1 == 120, "20dp downward swipe emits +120 (got " + d1 + ")");

        // 6. Throttling & coalescing within 32ms window, then emitting on next interval.
        int throttled = g.move(400, 500 + 38 * d, 540, 500 + 38 * d, 150, true, 1f); // +10ms < 32ms
        check(throttled == 0, "within 32ms throttle window buffers delta");
        int coalesced = g.move(400, 500 + 56 * d, 540, 500 + 56 * d, 180, true, 1f); // +40ms >= 32ms
        check(coalesced == 240, "coalesced two notches emit +240 (got " + coalesced + ")");

        // 7. Upward swipe emits negative WHEEL_DELTA (moves -20dp to cross the +2dp residual cleanly).
        int up = g.move(400, 500 + 36 * d, 540, 500 + 36 * d, 220, true, 1f);
        check(up == -120, "upward swipe emits -120 (got " + up + ")");

        // 8. Extreme fast swipe clamps strictly to [-1200, 1200].
        int hugeDown = g.move(400, 500 + 800 * d, 540, 500 + 800 * d, 260, true, 1f);
        check(hugeDown == 1200, "extreme positive swipe clamped to +1200 (got " + hugeDown + ")");
        int hugeUp = g.move(400, 500 - 800 * d, 540, 500 - 800 * d, 300, true, 1f);
        check(hugeUp == -1200, "extreme negative swipe clamped to -1200 (got " + hugeUp + ")");

        // 9. finish(false) flushes pending buffered notch; finish(true) cancels without flushing.
        g.move(400, 500 - 818 * d, 540, 500 - 818 * d, 310, true, 1f); // buffered -120 within throttle window
        int flushed = g.finish(false);
        check(flushed == -120 && !g.isWheel(), "finish(false) flushes buffered notch and resets");

        g.begin(400, 500, 540, 500, 400, true, 1f);
        g.move(400, 500 + 20 * d, 540, 500 + 20 * d, 440, true, 1f);
        g.move(400, 500 + 38 * d, 540, 500 + 38 * d, 450, true, 1f);
        check(g.finish(true) == 0 && !g.isWheel(), "finish(true) cancels buffered notch");

        // 10. Static clamp helper & wire encoding sanity check matching src/service.cpp Op::input (op=22).
        check(DesktopView.WheelGesture.clampDelta(9999) == 1200, "clamp upper bound");
        check(DesktopView.WheelGesture.clampDelta(-9999) == -1200, "clamp lower bound");
        byte[] packet = ByteBuffer.allocate(20).putInt(3).putInt(-240).putInt(0).putInt(0).putInt(1).array();
        ByteBuffer view = ByteBuffer.wrap(packet);
        check(packet.length == 20 && view.getInt() == 3 && view.getInt() == -240 &&
              view.getInt() == 0 && view.getInt() == 0 && view.getInt() == 1, "Op::input wheel wire layout");
    }

    public static void main(String[] args) {
        testStatusEquality();
        testQualityPublisherDedupAndCoalesce();
        testWheelGestureBoundaries();
        System.out.println(checks + " frontend upgrade checks passed (WP-PERF-02 + WheelGesture)");
    }
}
