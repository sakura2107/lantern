package org.lantern.client;

import android.view.*;
import android.widget.*;
import java.util.Locale;

/**
 * Desktop page. The preview frame is where the shared video pane is aligned; below it four
 * independent switches (picture, control, sound, local hide) and the desktop tools.
 */
final class DesktopPage {
    final AppShell shell;
    final MainActivity a;
    final LinearLayout view, controls;
    final FrameLayout preview;
    final ScrollView controlScroll;
    final LinearLayout empty;
    final ImageView emptyIcon;
    final TextView emptyTitle, emptyDetail;
    final Button emptyAction;
    final Tile picture, control, sound, hide;
    final TextView qualityValue, monitorValue, timingValue;
    boolean landscape;

    DesktopPage(AppShell s) {
        shell = s;
        a = s.a;
        view = Ui.column(a);
        view.setPadding(Theme.dp(16), Theme.dp(4), Theme.dp(16), 0);

        preview = new FrameLayout(a) {
            @Override protected void onMeasure(int w, int h) {
                // Portrait: a 16:9 frame, never shorter than the empty state needs.
                if (!landscape) {
                    int width = MeasureSpec.getSize(w);
                    h = MeasureSpec.makeMeasureSpec(Math.max(width * 9 / 16, Theme.dp(206)), MeasureSpec.EXACTLY);
                }
                super.onMeasure(w, h);
            }
            @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
                super.onSizeChanged(w, h, ow, oh);
                // Short frames (landscape on small phones) drop the icon so the action stays visible.
                post(() -> emptyIcon.setVisibility(h < Theme.dp(210) ? View.GONE : View.VISIBLE));
            }
        };
        preview.setBackground(Ui.gradient(Theme.dark ? 0xff0a0f17 : 0xff161f2c, Theme.dark ? 0xff05080d : 0xff0d131c,
                                          Theme.dark ? Theme.RIM : 0x26ffffff, 1f, 24));
        preview.setElevation(Theme.dp(4));
        empty = Ui.column(a);
        empty.setGravity(Gravity.CENTER);
        empty.setPadding(Theme.dp(26), Theme.dp(18), Theme.dp(26), Theme.dp(18));
        emptyIcon = new ImageView(a);
        emptyIcon.setScaleType(ImageView.ScaleType.CENTER);
        emptyIcon.setImageDrawable(new Glyph(Glyph.DESKTOP, 0xd9ffffff, Theme.dp(28)));
        emptyIcon.setBackground(Ui.outline(0x1affffff, 0x26ffffff, 1f, 32));
        empty.addView(emptyIcon, Ui.lp(Theme.dp(56), Theme.dp(56)));
        emptyTitle = Ui.medium(a, S.PREVIEW_OFF, 17, 0xffffffff);
        emptyTitle.setGravity(Gravity.CENTER);
        emptyTitle.setPadding(0, Theme.dp(12), 0, Theme.dp(4));
        empty.addView(emptyTitle);
        emptyDetail = Ui.text(a, S.PREVIEW_OFF_DETAIL, 13, 0xa8ffffff);
        emptyDetail.setGravity(Gravity.CENTER);
        emptyDetail.setLineSpacing(0, 1.2f);
        empty.addView(emptyDetail);
        emptyAction = Ui.button(a, S.START_PICTURE, Ui.PRIMARY, a::toggleDesktop);
        emptyAction.setTextColor(0xff12171e);
        emptyAction.setBackground(Ui.press(Ui.round(Theme.LAMP, 24), 24));
        emptyAction.setPadding(Theme.dp(24), 0, Theme.dp(24), 0);
        Ui.id(emptyAction, "desktop.subscribe.empty");
        LinearLayout.LayoutParams ep = Ui.lp(-2, Theme.dp(44));
        ep.topMargin = Theme.dp(16);
        empty.addView(emptyAction, ep);
        preview.addView(empty, new FrameLayout.LayoutParams(-1, -1));

        controls = Ui.column(a);
        controls.setPadding(0, Theme.dp(12), 0, Theme.dp(18));
        LinearLayout grid1 = Ui.row(a), grid2 = Ui.row(a);
        picture = new Tile(a, Glyph.DESKTOP, S.TILE_PICTURE, a::toggleDesktop);
        control = new Tile(a, Glyph.POINTER, S.TILE_CONTROL, a::toggleControl);
        sound = new Tile(a, Glyph.SPEAKER, S.TILE_SOUND, a::toggleAudio);
        hide = new Tile(a, Glyph.EYE_OFF, S.TILE_HIDE, a::toggleDisplay);
        Ui.id(picture, "desktop.subscribe");
        Ui.id(control, "desktop.control");
        Ui.id(sound, "desktop.audio");
        Ui.id(hide, "desktop.hide");
        addTile(grid1, picture, false); addTile(grid1, control, true);
        addTile(grid2, sound, false); addTile(grid2, hide, true);
        controls.addView(grid1, Ui.lp(-1, -2));
        LinearLayout.LayoutParams g2 = Ui.lp(-1, -2);
        g2.topMargin = Theme.dp(10);
        controls.addView(grid2, g2);

        LinearLayout toolCard = Ui.column(a);
        toolCard.setBackground(Ui.card(22));
        toolCard.setElevation(Theme.dp(1));
        toolCard.setPadding(Theme.dp(8), Theme.dp(8), Theme.dp(8), Theme.dp(8));
        LinearLayout tools1 = Ui.row(a), tools2 = Ui.row(a);
        tools2.setPadding(0, Theme.dp(4), 0, 0);
        tool(tools1, Glyph.EXPAND, S.TOOL_FULLSCREEN, null, () -> {
            if (a.desktopLayout != null && a.desktopLayout.visible) a.desktopLayout.setFullscreen(true);
            else a.status(S.NEED_PICTURE);
        }, "desktop.fullscreen.tool");
        tool(tools1, Glyph.KEYBOARD, S.TOOL_KEYBOARD, null, a::showKeyboard, "desktop.keyboard");
        qualityValue = tool(tools1, Glyph.SLIDERS, S.TOOL_QUALITY, "", a::showQuality, "desktop.quality");
        monitorValue = tool(tools1, Glyph.MONITORS, S.TOOL_MONITOR, "", () -> a.desktopTools.queryMonitors(a.credentials), "desktop.monitor");
        tool(tools2, Glyph.CLIPBOARD, S.TOOL_CLIPBOARD, null, a::showClipboard, "desktop.clipboard");
        tool(tools2, Glyph.COMMAND, S.TOOL_SHORTCUTS, null, a::showShortcuts, "desktop.shortcuts");
        timingValue = tool(tools2, Glyph.CLOCK, S.TOOL_TIMING, "", a::showTiming, "desktop.timing");
        TextView privacy = tool(tools2, Glyph.SHIELD, S.TOOL_PRIVACY, S.UNSUPPORTED, () ->
            Sheet.info(a, S.PRIVACY_TITLE, S.PRIVACY_EXPLAIN), "desktop.privacy");
        ((View)privacy.getParent()).setAlpha(0.55f);
        toolCard.addView(tools1, Ui.lp(-1, -2));
        toolCard.addView(tools2, Ui.lp(-1, -2));
        LinearLayout.LayoutParams tcp = Ui.lp(-1, -2);
        tcp.topMargin = Theme.dp(12);
        controls.addView(toolCard, tcp);

        controlScroll = new ScrollView(a);
        controlScroll.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);
        controlScroll.addView(controls);
        orientation(a.getResources().getConfiguration().orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE);
    }

    void addTile(LinearLayout row, Tile tile, boolean gap) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, -2, 1);
        if (gap) p.setMarginStart(Theme.dp(12));
        row.addView(tile, p);
    }

    TextView tool(LinearLayout row, int glyph, String label, String value, Runnable action, String id) {
        LinearLayout t = Ui.column(a);
        t.setGravity(Gravity.CENTER_HORIZONTAL);
        t.setPadding(Theme.dp(2), Theme.dp(8), Theme.dp(2), Theme.dp(6));
        ImageView icon = Ui.glyphTile(a, glyph, Theme.INK, Theme.INSET, 46, 15);
        icon.setBackground(Ui.outline(Theme.INSET, Theme.RIM, 1f, 15));
        t.addView(icon, Ui.lp(Theme.dp(46), Theme.dp(46)));
        TextView name = Ui.medium(a, label, 12, Theme.INK);
        name.setGravity(Gravity.CENTER);
        name.setPadding(0, Theme.dp(7), 0, 0);
        Ui.oneLine(name);
        t.addView(name);
        TextView sub = Ui.numeric(Ui.text(a, value == null ? " " : value, 10.5f, Theme.MUTED));
        sub.setGravity(Gravity.CENTER);
        sub.setPadding(0, Theme.dp(1), 0, 0);
        Ui.oneLine(sub);
        sub.setVisibility(value == null ? View.INVISIBLE : View.VISIBLE);
        t.addView(sub);
        t.setBackground(Ui.press(Ui.round(0, 16), 16));
        t.setOnClickListener(v -> action.run());
        t.setFocusable(true);
        t.setContentDescription(label);
        Ui.role(t, Button.class);
        Ui.bounce(t, 0.94f);
        Ui.id(t, id);
        row.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
        return sub;
    }

    /** Portrait stacks preview over controls; landscape places them side by side. */
    void orientation(boolean wide) {
        landscape = wide;
        view.removeAllViews();
        if (controlScroll.getParent() != null) ((ViewGroup)controlScroll.getParent()).removeView(controlScroll);
        if (preview.getParent() != null) ((ViewGroup)preview.getParent()).removeView(preview);
        if (wide) {
            LinearLayout side = Ui.row(a);
            side.setGravity(Gravity.TOP);
            LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(0, -1, 1);
            pp.bottomMargin = Theme.dp(12);
            side.addView(preview, pp);
            LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(Theme.dp(340), -1);
            cp.setMarginStart(Theme.dp(14));
            side.addView(controlScroll, cp);
            view.addView(side, new LinearLayout.LayoutParams(-1, 0, 1));
        } else {
            view.addView(preview, Ui.lp(-1, -2));
            view.addView(controlScroll, new LinearLayout.LayoutParams(-1, 0, 1));
        }
    }

    /** Called by MainActivity whenever any desktop capability changes. */
    void update(boolean subscribed, boolean display, boolean controlRequested, boolean controlOn, int audio,
                String profile, int monitor, boolean synchronize) {
        picture.set(subscribed ? Tile.ON : Tile.OFF, subscribed ? (display ? S.PICTURE_ON : S.PICTURE_ON_HIDDEN) : S.PICTURE_OFF);
        control.set(controlOn ? Tile.ON : controlRequested ? Tile.PENDING : Tile.OFF,
                    controlOn ? S.CONTROL_ON : controlRequested ? S.CONTROL_PENDING : S.CONTROL_OFF);
        switch (audio) {
        case MainActivity.AUDIO_ON: sound.set(Tile.ON, S.SOUND_ON); sound.setGlyph(Glyph.SPEAKER); break;
        case MainActivity.AUDIO_CONNECTING: sound.set(Tile.PENDING, S.SOUND_CONNECTING); sound.setGlyph(Glyph.SPEAKER); break;
        case MainActivity.AUDIO_RECONNECTING: sound.set(Tile.PENDING, S.SOUND_RECONNECTING); sound.setGlyph(Glyph.SPEAKER); break;
        default: sound.set(Tile.OFF, S.SOUND_OFF); sound.setGlyph(Glyph.SPEAKER_OFF);
        }
        hide.set(display ? Tile.OFF : Tile.ON, display ? S.HIDE_OFF : S.HIDE_ON);
        qualityValue.setText(profile);
        monitorValue.setText(String.format(Locale.ROOT, S.MONITOR_N, monitor + 1));
        timingValue.setText(synchronize ? S.TIMING_SYNC_SHORT : S.TIMING_FAST_SHORT);

        boolean showVideo = subscribed && display;
        empty.setVisibility(showVideo ? View.GONE : View.VISIBLE);
        if (!subscribed) {
            emptyIcon.setImageDrawable(new Glyph(Glyph.DESKTOP, 0xd9ffffff, Theme.dp(28)));
            emptyTitle.setText(S.PREVIEW_OFF);
            emptyDetail.setText(shell.connected || a.credentials != null ? S.PREVIEW_OFF_DETAIL : S.PREVIEW_NEED_DEVICE);
            emptyAction.setText(S.START_PICTURE);
            emptyAction.setOnClickListener(v -> a.toggleDesktop());
        } else if (!display) {
            emptyIcon.setImageDrawable(new Glyph(Glyph.EYE_OFF, Theme.LAMP, Theme.dp(28)));
            emptyTitle.setText(S.PREVIEW_HIDDEN);
            emptyDetail.setText(S.PREVIEW_HIDDEN_DETAIL);
            emptyAction.setText(S.SHOW_PICTURE);
            emptyAction.setOnClickListener(v -> a.toggleDisplay());
        }
    }

    void connectionChanged() {
        if (!a.subscribed.get()) emptyDetail.setText(shell.connected || a.credentials != null ? S.PREVIEW_OFF_DETAIL : S.PREVIEW_NEED_DEVICE);
    }
}
