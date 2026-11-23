package org.lantern.client;

import android.content.res.Configuration;
import android.os.Build;
import android.text.TextUtils;
import android.view.*;
import android.view.animation.DecelerateInterpolator;
import android.view.animation.OvershootInterpolator;
import android.widget.*;
import java.util.*;

/**
 * Presentation shell: app bar, four pages, bottom navigation and the transient message bar.
 * Pages keep their views and state while navigation changes; business work stays in MainActivity.
 */
final class AppShell {
    static final int DEVICE = 0, DESKTOP = 1, FILES = 2, TRANSFERS = 3;
    /** Connection presentation states. */
    static final int UNPAIRED = 0, SAVED = 1, CONNECTING = 2, CONNECTED = 3, FAILED = 4, INVALID = 5;

    final MainActivity a;
    final FrameLayout window, pages;
    final LinearLayout root;
    final View[] page = new View[4];
    final View[] nav = new View[4];
    final TextView title, connection, status;
    final View connectionDot, snackbar;
    final LinearLayout connectionChip;
    final TextView[] navLabel = new TextView[4];
    final Glyph[] navGlyph = new Glyph[4];
    final View[] navPill = new View[4];
    final TextView transferBadge;
    LinearLayout navigation;
    int lastBadge = -1;
    boolean ready;

    final DevicePage device;
    final DesktopPage desktop;
    final FilesPage files;
    final TransfersPage transfers;

    // Compatibility handles used by MainActivity and the device validation runner.
    final EditText host, pin, secret, path;
    final TextView deviceState;
    final Button pair;
    final ListView fileList, taskList;
    final LinearLayout desktopPage;

    int selected = DEVICE, connectionState = UNPAIRED;
    boolean connected;
    final Runnable hideSnackbar;

    AppShell(MainActivity activity) {
        a = activity;
        Theme.init(a);
        window = new FrameLayout(a);
        window.setBackgroundColor(Theme.BG);
        root = Ui.column(a);
        root.setBackgroundColor(Theme.BG);
        root.setFocusableInTouchMode(true);
        window.addView(root, new FrameLayout.LayoutParams(-1, -1));
        root.setOnApplyWindowInsetsListener((v, i) -> {
            int left = i.getSystemWindowInsetLeft(), right = i.getSystemWindowInsetRight();
            if (Build.VERSION.SDK_INT >= 28 && i.getDisplayCutout() != null) {
                left = Math.max(left, i.getDisplayCutout().getSafeInsetLeft());
                right = Math.max(right, i.getDisplayCutout().getSafeInsetRight());
            }
            int nav = i.getSystemWindowInsetBottom(), ime = 0;
            if (Build.VERSION.SDK_INT >= 30) {
                nav = i.getInsets(WindowInsets.Type.navigationBars()).bottom;
                ime = i.getInsets(WindowInsets.Type.ime()).bottom;
            }
            // The navigation surface extends behind the system bar; the keyboard pushes everything up.
            v.setPadding(left, i.getSystemWindowInsetTop(), right, Math.max(0, ime - nav));
            if (navigation != null) navigation.setPadding(Theme.dp(6), Theme.dp(6), Theme.dp(6), Theme.dp(4) + nav);
            return i;
        });

        // App bar: page title on the left, live connection pill on the right.
        LinearLayout bar = Ui.row(a);
        bar.setPadding(Theme.dp(20), Theme.dp(12), Theme.dp(16), Theme.dp(8));
        bar.setMinimumHeight(Theme.dp(62));
        title = Ui.medium(a, S.APP_NAME, 26, Theme.INK);
        title.setLetterSpacing(-0.02f);
        Ui.oneLine(title);
        title.setAccessibilityHeading(true);
        bar.addView(title, Ui.weight(1));
        LinearLayout chip = Ui.row(a);
        chip.setPadding(Theme.dp(11), 0, Theme.dp(13), 0);
        chip.setMinimumHeight(Theme.dp(36));
        chip.setBackground(Ui.press(Ui.outline(Theme.SURFACE, Theme.RIM, 1f, 50), 50));
        chip.setElevation(Theme.dp(1));
        connectionDot = new View(a);
        chip.addView(connectionDot, Ui.lp(Theme.dp(8), Theme.dp(8)));
        connection = Ui.medium(a, S.STATE_UNPAIRED, 12.5f, Theme.INK);
        Ui.oneLine(connection);
        connection.setMaxWidth(Theme.dp(170));
        LinearLayout.LayoutParams cp = Ui.lp(-2, -2);
        cp.setMarginStart(Theme.dp(7));
        chip.addView(connection, cp);
        chip.setOnClickListener(v -> select(DEVICE));
        chip.setFocusable(true);
        Ui.role(chip, Button.class);
        Ui.bounce(chip, 0.96f);
        Ui.id(chip, "shell.connection");
        connectionChip = chip;
        bar.addView(chip, Ui.lp(-2, -2));
        root.addView(bar, Ui.lp(-1, -2));

        pages = new FrameLayout(a);
        root.addView(pages, new LinearLayout.LayoutParams(-1, 0, 1));

        device = new DevicePage(this);
        desktop = new DesktopPage(this);
        files = new FilesPage(this);
        transfers = new TransfersPage(this);
        page[DEVICE] = device.view;
        page[DESKTOP] = desktop.view;
        page[FILES] = files.view;
        page[TRANSFERS] = transfers.view;
        for (View view : page) {
            pages.addView(view, new FrameLayout.LayoutParams(-1, -1));
            view.setVisibility(View.GONE);
        }
        host = device.host; pin = device.pin; secret = device.secret; pair = device.pair; deviceState = device.detail;
        path = files.path; fileList = files.list; taskList = transfers.list; desktopPage = desktop.view;

        // Transient message bar above the navigation. Errors stay longer; tap dismisses.
        LinearLayout bubble = Ui.row(a);
        bubble.setPadding(Theme.dp(15), Theme.dp(12), Theme.dp(16), Theme.dp(12));
        bubble.setBackground(Theme.dark ? Ui.outline(Theme.SURFACE_RAISED, Theme.RIM, 1f, 18) : Ui.round(Theme.INK, 18));
        View snackDot = new View(a);
        snackDot.setBackground(Ui.round(Theme.LAMP, 4));
        LinearLayout.LayoutParams sdp = Ui.lp(Theme.dp(7), Theme.dp(7));
        sdp.setMarginEnd(Theme.dp(10));
        bubble.addView(snackDot, sdp);
        status = Ui.text(a, "", 13.5f, Theme.dark ? Theme.INK : Theme.ON_INK);
        status.setMaxLines(3);
        status.setEllipsize(TextUtils.TruncateAt.END);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        bubble.addView(status, Ui.weight(1));
        FrameLayout.LayoutParams sp = new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM);
        sp.setMargins(Theme.dp(14), 0, Theme.dp(14), Theme.dp(12));
        pages.addView(bubble, sp);
        bubble.setVisibility(View.GONE);
        bubble.setElevation(Theme.dp(8));
        snackbar = bubble;
        hideSnackbar = () -> snackbar.animate().alpha(0f).translationY(Theme.dp(10)).setDuration(160)
            .withEndAction(() -> snackbar.setVisibility(View.GONE)).start();
        bubble.setOnClickListener(v -> { v.removeCallbacks(hideSnackbar); hideSnackbar.run(); });

        // Bottom navigation.
        View navBorder = new View(a);
        navBorder.setBackgroundColor(Theme.dark ? Theme.RIM : Theme.LINE);
        root.addView(navBorder, Ui.lp(-1, Math.max(1, Theme.dp(0.75f))));
        navigation = Ui.row(a);
        navigation.setBackgroundColor(Theme.SURFACE);
        navigation.setPadding(Theme.dp(8), Theme.dp(6), Theme.dp(8), Theme.dp(4));
        String[] labels = {S.TAB_DEVICE, S.TAB_DESKTOP, S.TAB_FILES, S.TAB_TRANSFERS};
        int[] glyphs = {Glyph.PHONE, Glyph.DESKTOP, Glyph.FOLDER, Glyph.TRANSFER};
        String[] ids = {"nav.device", "nav.desktop", "nav.files", "nav.transfers"};
        TextView badge = null;
        for (int i = 0; i < 4; i++) {
            final int tab = i;
            LinearLayout item = Ui.column(a);
            item.setGravity(Gravity.CENTER_HORIZONTAL);
            item.setPadding(0, Theme.dp(5), 0, Theme.dp(3));
            FrameLayout iconBox = new FrameLayout(a);
            View pill = new View(a);
            pill.setBackground(Ui.outline(Theme.NAV_PILL, Theme.alpha(Theme.LAMP, 0.28f), 1f, 16));
            pill.setAlpha(0f);
            iconBox.addView(pill, new FrameLayout.LayoutParams(Theme.dp(58), Theme.dp(32), Gravity.CENTER));
            ImageView icon = new ImageView(a);
            navGlyph[i] = new Glyph(glyphs[i], Theme.MUTED, Theme.dp(21));
            icon.setImageDrawable(navGlyph[i]);
            iconBox.addView(icon, new FrameLayout.LayoutParams(Theme.dp(21), Theme.dp(21), Gravity.CENTER));
            if (i == TRANSFERS) {
                badge = Ui.numeric(Ui.medium(a, "", 10, Theme.ON_LAMP));
                badge.setGravity(Gravity.CENTER);
                badge.setMinWidth(Theme.dp(17));
                badge.setPadding(Theme.dp(4), 0, Theme.dp(4), 0);
                badge.setBackground(Ui.outline(Theme.LAMP, Theme.SURFACE, 1.5f, 9));
                badge.setVisibility(View.GONE);
                iconBox.addView(badge, new FrameLayout.LayoutParams(-2, Theme.dp(17), Gravity.CENTER));
                badge.setTranslationX(Theme.dp(14));
                badge.setTranslationY(-Theme.dp(9));
            }
            item.addView(iconBox, Ui.lp(-1, Theme.dp(32)));
            TextView label = Ui.medium(a, labels[i], 11.5f, Theme.MUTED);
            label.setPadding(0, Theme.dp(4), 0, 0);
            label.setGravity(Gravity.CENTER_HORIZONTAL);
            item.addView(label, Ui.lp(-1, -2));
            item.setBackground(Ui.press(Ui.round(0, 16), 16));
            item.setOnClickListener(v -> select(tab));
            item.setFocusable(true);
            item.setContentDescription(labels[i]);
            Ui.role(item, android.widget.RadioButton.class);
            Ui.id(item, ids[i]);
            navigation.addView(item, new LinearLayout.LayoutParams(0, Theme.dp(64), 1));
            nav[i] = item; navLabel[i] = label; navPill[i] = pill;
        }
        transferBadge = badge;
        root.addView(navigation, Ui.lp(-1, -2));
        setConnection(UNPAIRED, "");
        select(DEVICE);
        ready = true;
        root.requestFocus(); // keep the soft keyboard closed until a field is tapped
    }

    void select(int tab) {
        int previous = selected;
        if (tab != DESKTOP) {
            if (a.screen != null) a.screen.cancelDrag();
            if (a.keyboard != null) a.keyboard.close();
        }
        selected = tab;
        boolean animate = ready && tab != previous;
        int activeGlyphColor = Theme.dark ? Theme.LAMP : Theme.LAMP_TEXT;
        for (int i = 0; i < 4; i++) {
            boolean on = i == tab;
            View p = page[i];
            p.animate().cancel();
            if (on) {
                p.setVisibility(View.VISIBLE);
                if (animate) {
                    p.setAlpha(0f);
                    p.setTranslationY(Theme.dp(8));
                    p.animate().alpha(1f).translationY(0f).setDuration(145)
                        .setInterpolator(new DecelerateInterpolator(1.8f)).start();
                } else {
                    p.setAlpha(1f);
                    p.setTranslationY(0f);
                }
            } else {
                p.setAlpha(1f);
                p.setTranslationY(0f);
                p.setVisibility(View.GONE);
            }
            View pill = navPill[i];
            pill.animate().cancel();
            if (on) {
                if (animate) {
                    pill.setScaleX(0.68f);
                    pill.setAlpha(0f);
                    pill.animate().scaleX(1f).alpha(1f).setDuration(180)
                        .setInterpolator(new OvershootInterpolator(1.3f)).start();
                } else {
                    pill.setScaleX(1f);
                    pill.setAlpha(1f);
                }
            } else {
                if (animate && pill.getAlpha() > 0f) {
                    pill.animate().alpha(0f).scaleX(0.75f).setDuration(110).start();
                } else {
                    pill.setAlpha(0f);
                    pill.setScaleX(0.75f);
                }
            }
            navGlyph[i].setColor(on ? activeGlyphColor : Theme.MUTED);
            navLabel[i].setTextColor(on ? Theme.INK : Theme.MUTED);
            nav[i].setSelected(on);
        }
        title.setText(new String[] {S.APP_NAME, S.TITLE_DESKTOP, S.TITLE_FILES, S.TITLE_TRANSFERS}[tab]);
        if (a.desktopLayout != null) a.desktopLayout.pageVisible(tab == DESKTOP);
        if (tab == TRANSFERS) a.renderTransfers();
        if (tab == FILES) files.updateEmpty();
    }

    /** Message bar. Longer and error-like messages stay a little longer. */
    void notice(String text) {
        status.setText(text);
        snackbar.removeCallbacks(hideSnackbar);
        snackbar.animate().cancel();
        boolean wasHidden = snackbar.getVisibility() != View.VISIBLE;
        snackbar.setVisibility(View.VISIBLE);
        if (wasHidden) {
            snackbar.setAlpha(0f);
            snackbar.setTranslationY(Theme.dp(12));
            snackbar.animate().alpha(1f).translationY(0f).setDuration(180)
                .setInterpolator(new DecelerateInterpolator(1.8f)).start();
        } else {
            snackbar.setAlpha(1f);
            snackbar.setTranslationY(0f);
        }
        snackbar.postDelayed(hideSnackbar, Math.min(7000, 3200 + text.length() * 45L));
    }

    void setConnection(int state, String address) {
        connectionState = state;
        connected = state == CONNECTED;
        String[] chips = {S.STATE_UNPAIRED, S.STATE_SAVED, S.STATE_CONNECTING, S.STATE_CONNECTED, S.STATE_FAILED, S.STATE_INVALID};
        connection.setText(chips[state]);
        int dot = state == CONNECTED ? Theme.LAMP : state == CONNECTING ? Theme.LAMP_SOFT
                  : state == FAILED || state == INVALID ? Theme.DANGER : Theme.FAINT;
        connectionDot.setBackground(Ui.round(dot, 4));
        int chipFill = state == CONNECTED ? (Theme.dark ? 0xff1c1c18 : 0xfffff8eb)
                       : state == FAILED || state == INVALID ? Theme.DANGER_SOFT : Theme.SURFACE;
        int chipStroke = state == CONNECTED ? Theme.LAMP_GLOW
                         : state == CONNECTING ? Theme.alpha(Theme.LAMP, 0.35f)
                         : state == FAILED || state == INVALID ? Theme.alpha(Theme.DANGER, 0.35f) : Theme.RIM;
        connectionChip.setBackground(Ui.press(Ui.outline(chipFill, chipStroke, 1f, 50), 50));
        connection.setTextColor(state == CONNECTED ? Theme.LAMP_TEXT
                                : state == FAILED || state == INVALID ? Theme.DANGER : Theme.INK);
        device.setConnection(state, address);
        desktop.connectionChanged();
        files.updateEmpty();
    }

    /** Kept for the validation runner: marks the current credentials as verified. */
    void connected(String address) { setConnection(CONNECTED, address); }

    void renderFiles(List<Remote.Entry> items, boolean selecting) { files.render(items, selecting); }

    void transferBadge(int active) {
        if (active == lastBadge) return;
        lastBadge = active;
        transferBadge.setVisibility(active > 0 ? View.VISIBLE : View.GONE);
        if (active > 0) transferBadge.setText(active > 9 ? "9+" : String.valueOf(active));
        nav[TRANSFERS].setContentDescription(active > 0 ? S.TAB_TRANSFERS + "，" + String.format(Locale.ROOT, S.ACTIVE_COUNT, active) : S.TAB_TRANSFERS);
    }

    void configurationChanged(Configuration config) {
        desktop.orientation(config.orientation == Configuration.ORIENTATION_LANDSCAPE);
    }
}
