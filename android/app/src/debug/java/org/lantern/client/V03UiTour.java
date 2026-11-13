package org.lantern.client;
import android.os.SystemClock;
/**
 * Debug-only screenshot tour of the v0.3 phone UI: quality sheet, link banner states (embedded and
 * fullscreen), movie playback choice and the track picker. States are injected into the presentation
 * layer only; no other app is opened. Screenshots land in files/ui-validation for the PC to pull.
 */
final class V03UiTour {
    static String shell(DeviceTestRunner t, String command) throws Exception {
        try (android.os.ParcelFileDescriptor fd = t.getUiAutomation().executeShellCommand(command);
             java.io.InputStream in = new android.os.ParcelFileDescriptor.AutoCloseInputStream(fd)) {
            return new String(in.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8);
        }
    }
    /** Pictures the compositor actually presented from the video SurfaceView over its last ~127 frames. */
    static double presentedFps(DeviceTestRunner t, MainActivity a) throws Exception {
        String name = null;
        for (String line : shell(t, "dumpsys SurfaceFlinger --list").split("\n"))
            if (line.contains("SurfaceView[org.lantern.client") && line.contains("(BLAST)")) {
                String l = line.trim(); int open = l.indexOf('{'), cut = l.indexOf(" parentId=");
                name = l.substring(open + 1, cut > 0 ? cut : l.length());
            }
        return presentedFpsOf(t, a, name);
    }
    static double presentedFpsOf(DeviceTestRunner t, MainActivity a, String name) throws Exception {
        if (name == null) return -1;
        java.io.File script = new java.io.File(a.getExternalFilesDir(null), "latency.sh");
        java.nio.file.Files.write(script.toPath(), ("dumpsys SurfaceFlinger --latency \"" + name + "\"\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
        String out;
        try { out = shell(t, "sh " + script.getAbsolutePath()); } finally { script.delete(); }
        java.util.List<Long> times = new java.util.ArrayList<>();
        String[] lines = out.split("\n");
        for (int i = 1; i < lines.length; i++) {
            String[] f = lines[i].trim().split("\\s+");
            if (f.length == 3) { long v = Long.parseLong(f[1]); if (v > 0 && v < Long.MAX_VALUE) times.add(v); }
        }
        if (times.size() < 2) return 0;
        java.util.Collections.sort(times);
        return (times.size() - 1) * 1e9 / (times.get(times.size() - 1) - times.get(0));
    }
    /** Compositor lines about refresh/frame-rate decisions (display mode, votes, Lantern layers). */
    static org.json.JSONArray sfRates(DeviceTestRunner t) throws Exception {
        org.json.JSONArray out = new org.json.JSONArray();
        for (String line : shell(t, "dumpsys SurfaceFlinger").split("\n")) {
            String l = line.trim(), lower = l.toLowerCase(java.util.Locale.ROOT);
            if ((lower.contains("lantern") && (lower.contains("frame") || lower.contains("rate") || lower.contains("vote")))
                || lower.startsWith("refreshrates") || lower.contains("apprequestrefreshrates") || lower.contains("primaryrefreshrates")
                || lower.contains("rendermode") || lower.contains("renderrate") || lower.contains("framerateoverride")
                || (lower.contains("layer") && lower.contains("vote")))
                if (out.length() < 60) out.put(l.length() > 220 ? l.substring(0, 220) : l);
        }
        return out;
    }
    /** One real (system-injected) finger swipe across the middle of the fullscreen picture, ~300 ms. */
    static void swipe(DeviceTestRunner t, MainActivity a) throws Exception {
        android.app.UiAutomation ui = t.getUiAutomation();
        int w = a.screen.getWidth(), h = a.screen.getHeight();
        int[] at = new int[2]; a.screen.getLocationOnScreen(at);
        float y = at[1] + h / 2f, x0 = at[0] + w * 0.35f, x1 = at[0] + w * 0.65f;
        long down = android.os.SystemClock.uptimeMillis();
        ui.injectInputEvent(motion(down, down, android.view.MotionEvent.ACTION_DOWN, x0, y), true);
        for (int i = 1; i <= 10; i++) {
            Thread.sleep(25);
            float x = x0 + (x1 - x0) * i / 10f;
            ui.injectInputEvent(motion(down, android.os.SystemClock.uptimeMillis(), android.view.MotionEvent.ACTION_MOVE, x, y), true);
        }
        ui.injectInputEvent(motion(down, android.os.SystemClock.uptimeMillis(), android.view.MotionEvent.ACTION_UP, x1, y), true);
        Thread.sleep(50);
    }
    static android.view.MotionEvent motion(long down, long time, int action, float x, float y) {
        android.view.MotionEvent e = android.view.MotionEvent.obtain(down, time, action, x, y, 0);
        e.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN);
        return e;
    }
    static void run(DeviceTestRunner t, MainActivity a, Remote.Credentials c) throws Exception {
        t.runOnMainSync(() -> { a.hardwareDesktop = true; a.subscribed.set(true); a.display.set(true); a.ui.select(AppShell.DESKTOP); a.updateDesktopPresentation(); });
        V03Validation.until(() -> a.videoSeen, 20000, "no desktop picture");
        Thread.sleep(2500); // screen limits load after the first frame
        t.runOnMainSync(() -> a.ui.select(AppShell.DESKTOP));
        t.snapshot(a, "tour-desktop");
        org.json.JSONArray rates = new org.json.JSONArray();
        // Real UI path, picture fullscreen; q[2] = inject real touch swipes during the measurement.
        // presented_fps is read from the compositor (what the screen actually showed).
        for (int[] q : new int[][]{{1280, 120, 0}, {1920, 120, 0}, {2560, 120, 0}, {1920, 60, 0}, {1280, 120, 1}}) {
            t.runOnMainSync(() -> { a.qualityAuto = false; a.qualityWidth = q[0]; a.qualityFps = q[1]; a.applyQuality("tour"); a.desktopLayout.setFullscreen(true); });
            java.util.concurrent.atomic.AtomicBoolean touching = new java.util.concurrent.atomic.AtomicBoolean(q[2] == 1);
            Thread toucher = new Thread(() -> { while (touching.get()) try { swipe(t, a); } catch (Exception stop) { return; } });
            toucher.start();
            Thread.sleep(3000);
            long before = a.avcView.updateCount(), packets = a.video.received.get(), began = System.nanoTime();
            Thread.sleep(3000);
            double seconds = (System.nanoTime() - began) / 1e9;
            long received = a.video.received.get() - packets, shown = a.avcView.updateCount() - before;
            double appPeriodMs = a.video.vsync.period() / 1e6;
            double presented = presentedFps(t, a);
            Thread.sleep(1500); presented += presentedFps(t, a);
            Thread.sleep(1500); presented += presentedFps(t, a);
            presented /= 3;
            touching.set(false); toucher.join(3000);
            // Untouched, the app must keep the 120 Hz cadence (ColorOS would drop it to 60 Hz) and show
            // well above 60 fps at 120 (84-95 measured); 60 fps must stay near full (55-58 measured).
            t.check(appPeriodMs < 10 && presented > (q[1] == 120 ? 75 : 50), "picture presented at " + presented + " fps for " + q[0] + "@" + q[1]);
            DesktopSession.Status st = a.liveQuality;
            rates.put(new org.json.JSONObject().put("chosen", q[0] + "@" + q[1] + (q[2] == 1 ? " touch" : " no-touch"))
                .put("sent", st == null ? "none" : st.width + "x" + st.height + "@" + st.fps + " " + st.codec + " " + st.path + (st.constrained ? " constrained" : "") + (st.dropping ? " dropping" : ""))
                .put("received_fps", received / seconds)
                .put("shown_fps", shown / seconds)
                .put("presented_fps", presented).put("app_refresh_ms", appPeriodMs)
                .put("display_mode", a.preferredMode));
            // Untouched: the app must stay at the 120 Hz cadence (ColorOS would drop it to 60 Hz).
        }
        t.runOnMainSync(() -> a.desktopLayout.setFullscreen(false));
        t.report.put("ui_quality_rates", rates);

        t.runOnMainSync(a::showQuality);
        t.snapshot(a, "tour-quality");
        t.runOnMainSync(() -> Sheet.current.dismiss());
        t.runOnMainSync(() -> { a.videoLink = new ConnectionStatus.Snapshot(ConnectionStatus.Phase.RECOVERING, 4200); a.videoLinkAt = SystemClock.uptimeMillis(); a.linkChanged(); });
        t.check(a.desktopLayout.link.getVisibility() == android.view.View.VISIBLE, "recovering banner shown");
        t.snapshot(a, "tour-banner-recovering");
        t.runOnMainSync(() -> { a.videoLink = new ConnectionStatus.Snapshot(ConnectionStatus.Phase.DISCONNECTED, 31000); a.videoLinkAt = SystemClock.uptimeMillis(); a.linkChanged(); });
        t.check(a.desktopLayout.linkAction.getVisibility() == android.view.View.VISIBLE, "disconnected banner offers reconnect");
        t.snapshot(a, "tour-banner-lost");
        t.runOnMainSync(() -> a.desktopLayout.setFullscreen(true));
        Thread.sleep(800);
        t.runOnMainSync(a::linkChanged);
        t.snapshot(a, "tour-full-banner-lost");
        t.runOnMainSync(() -> {
            a.videoLink = null; a.qualityAuto = false; a.qualityWidth = 2560;
            a.liveQuality = new DesktopSession.Status(2560, 1440, 60, 8_000_000, "hevc", "tcp", 2560, true, false);
            a.constrainedSince = SystemClock.uptimeMillis() - 4000; a.linkChanged();
        });
        t.snapshot(a, "tour-full-banner-short");
        t.runOnMainSync(() -> { a.desktopLayout.setFullscreen(false); a.liveQuality = null; a.constrainedSince = 0; a.videoLink = null; a.linkChanged(); });
        Remote.Info info;
        try (Remote r = new Remote(c, 1, 2)) { info = r.stat("movie.mkv", false); }
        Remote.Entry movie = new Remote.Entry("movie.mkv", false, info.size, 0);
        t.runOnMainSync(() -> a.ui.select(AppShell.FILES));
        Thread.sleep(800);
        t.runOnMainSync(() -> a.ui.files.showFile(movie));
        t.snapshot(a, "tour-movie-sheet");
        Sheet fileSheet = Sheet.current;
        t.runOnMainSync(fileSheet::dismiss);
        t.runOnMainSync(() -> a.chooseTracks(movie, "movie.mkv"));
        V03Validation.until(() -> Sheet.current != fileSheet, 15000, "track picker not shown");
        t.snapshot(a, "tour-tracks");
        t.runOnMainSync(() -> Sheet.current.dismiss());
        // Saved computers: the verified connection is in the list with its secret and last entry.
        Computers.Saved saved = Computers.find(a, c.pin);
        t.check(saved != null && saved.secret(a).equals(c.token), "connected computer saved with its encrypted secret");
        t.check(!saved.lastHost.isEmpty() && saved.lastPort > 0, "last working entry remembered");
        t.check(NetworkDialer.WINNERS.containsKey(c.pin), "dialer tries the remembered entry first");
        t.runOnMainSync(() -> a.ui.select(AppShell.DEVICE));
        V03Validation.until(() -> a.ui.device.computersRow.getVisibility() == android.view.View.VISIBLE, 5000, "saved computers row hidden");
        t.snapshot(a, "tour-device-computers");
        t.runOnMainSync(a::showComputers);
        V03Validation.until(() -> Sheet.current != null && Sheet.current.dialog.isShowing(), 5000, "computers sheet not shown");
        Thread.sleep(500);
        t.snapshot(a, "tour-computers-sheet");
        t.runOnMainSync(() -> Sheet.current.dismiss());
        t.report.put("ui_tour_screens", 10);
    }
}
