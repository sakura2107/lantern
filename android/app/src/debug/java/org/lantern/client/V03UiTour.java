package org.lantern.client;
import android.os.SystemClock;
/**
 * Debug-only screenshot tour of the v0.3 phone UI: quality sheet, link banner states (embedded and
 * fullscreen), movie playback choice and the track picker. States are injected into the presentation
 * layer only; no other app is opened. Screenshots land in files/ui-validation for the PC to pull.
 */
final class V03UiTour {
    static void run(DeviceTestRunner t, MainActivity a, Remote.Credentials c) throws Exception {
        t.runOnMainSync(() -> { a.hardwareDesktop = true; a.subscribed.set(true); a.display.set(true); a.ui.select(AppShell.DESKTOP); a.updateDesktopPresentation(); });
        V03Validation.until(() -> a.videoSeen, 20000, "no desktop picture");
        Thread.sleep(2500); // screen limits load after the first frame
        t.runOnMainSync(() -> a.ui.select(AppShell.DESKTOP));
        t.snapshot(a, "tour-desktop");
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
