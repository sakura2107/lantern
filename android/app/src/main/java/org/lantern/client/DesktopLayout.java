package org.lantern.client;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Outline;
import android.graphics.Rect;
import android.os.Build;
import android.view.*;
import android.view.inputmethod.InputMethodManager;
import android.widget.*;

/**
 * Presentation only. The desktop view keeps the same parent across fullscreen and rotation:
 * the pane lives in the window and is aligned to an anchor inside the desktop page's preview.
 */
final class DesktopLayout {
    interface Actions {
        void quality(); void control(); void keyboard(); void audio(); void files(); void tools(); void rightClick();
    }
    final Activity activity;
    final FrameLayout outer, pane, anchor, toolbar;
    final LinearLayout page, toolRow;
    final TextView notice;
    final View fullButton, toolsButton;
    final ToolButton controlButton, audioButton, rightButton;
    boolean fullscreen, visible, onDesktopPage;
    int previousOrientation;
    final int gap;
    final Runnable hideNotice;

    DesktopLayout(Activity a, FrameLayout host, LinearLayout normal, FrameLayout preview, View image, Actions actions) {
        activity = a;
        outer = host;
        page = normal;
        gap = Theme.dp(10);
        anchor = new FrameLayout(a);
        preview.addView(anchor, new FrameLayout.LayoutParams(-1, -1));
        anchor.setVisibility(View.GONE);
        pane = new FrameLayout(a);
        pane.setBackgroundColor(Color.BLACK);
        pane.setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View v, Outline outline) {
                outline.setRoundRect(0, 0, v.getWidth(), v.getHeight(), fullscreen ? 0 : Theme.dp(22));
            }
        });
        pane.setClipToOutline(true);
        pane.addView(image, new FrameLayout.LayoutParams(-1, -1));
        image.setVisibility(View.VISIBLE);
        pane.setVisibility(View.GONE);
        outer.addView(pane, new FrameLayout.LayoutParams(1, 1));

        // Fullscreen toolbar: one floating strip, scrollable on narrow screens.
        toolbar = new FrameLayout(a);
        toolbar.setBackground(Ui.round(Theme.VIDEO_CHROME, 20));
        toolbar.setPadding(Theme.dp(4), Theme.dp(4), Theme.dp(4), Theme.dp(4));
        HorizontalScrollView scroll = new HorizontalScrollView(a);
        scroll.setHorizontalScrollBarEnabled(false);
        toolRow = Ui.row(a);
        scroll.addView(toolRow);
        toolbar.addView(scroll, new FrameLayout.LayoutParams(-2, -2));
        add(Glyph.SHRINK, S.FULL_EXIT, "full.exit", () -> setFullscreen(false));
        add(Glyph.ROTATE, S.FULL_ROTATE, "full.rotate", () -> activity.setRequestedOrientation(
                activity.getResources().getConfiguration().orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
                    ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE));
        divider();
        controlButton = add(Glyph.POINTER, S.TILE_CONTROL, "desktop.control", actions::control);
        rightButton = add(Glyph.MOUSE_RIGHT, S.FULL_RIGHT, "full.right", actions::rightClick);
        add(Glyph.KEYBOARD, S.TOOL_KEYBOARD, "full.keyboard", actions::keyboard);
        audioButton = add(Glyph.SPEAKER_OFF, S.TILE_SOUND, "desktop.audio", actions::audio);
        divider();
        add(Glyph.SLIDERS, S.TOOL_QUALITY, "full.quality", actions::quality);
        add(Glyph.TOOLS, S.MORE, "full.more", actions::tools);
        add(Glyph.FOLDER, S.TAB_FILES, "full.files", actions::files);
        add(Glyph.CLOSE, S.FULL_COLLAPSE, "full.collapse", () -> showToolbar(false));
        FrameLayout.LayoutParams bar = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        bar.setMargins(gap, gap, gap, gap);
        pane.addView(toolbar, bar);
        toolbar.setVisibility(View.GONE);

        ImageView full = new ImageView(a);
        full.setImageDrawable(new Glyph(Glyph.EXPAND, Color.WHITE, Theme.dp(20)));
        full.setScaleType(ImageView.ScaleType.CENTER);
        full.setBackground(Ui.press(Ui.round(Theme.VIDEO_CHROME, 18), 18));
        full.setContentDescription(S.TOOL_FULLSCREEN);
        full.setOnClickListener(v -> setFullscreen(true));
        Ui.role(full, Button.class);
        Ui.id(full, "desktop.fullscreen");
        FrameLayout.LayoutParams corner = new FrameLayout.LayoutParams(Theme.dp(40), Theme.dp(40), Gravity.TOP | Gravity.END);
        corner.setMargins(gap, gap, gap, gap);
        pane.addView(full, corner);
        fullButton = full;

        // Collapsed handle on the right edge, clear of the Windows taskbar and title-bar buttons.
        ImageView handle = new ImageView(a);
        handle.setImageDrawable(new Glyph(Glyph.TOOLS, Color.WHITE, Theme.dp(20)));
        handle.setScaleType(ImageView.ScaleType.CENTER);
        handle.setBackground(Ui.press(Ui.round(Theme.VIDEO_CHROME, 22), 22));
        handle.setAlpha(0.85f);
        handle.setContentDescription(S.FULL_TOOLS);
        handle.setOnClickListener(v -> showToolbar(true));
        Ui.role(handle, Button.class);
        Ui.id(handle, "full.tools");
        FrameLayout.LayoutParams edge = new FrameLayout.LayoutParams(Theme.dp(44), Theme.dp(44), Gravity.CENTER_VERTICAL | Gravity.END);
        edge.setMargins(gap, gap, gap, gap);
        pane.addView(handle, edge);
        handle.setVisibility(View.GONE);
        toolsButton = handle;

        notice = Ui.text(a, "", 13, Color.WHITE);
        notice.setMaxLines(2);
        notice.setEllipsize(android.text.TextUtils.TruncateAt.END);
        notice.setBackground(Ui.round(Theme.VIDEO_CHROME, 14));
        notice.setPadding(Theme.dp(14), Theme.dp(8), Theme.dp(14), Theme.dp(8));
        notice.setMaxWidth(Theme.dp(520));
        notice.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        FrameLayout.LayoutParams np = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        np.setMargins(gap * 2, gap, gap * 2, gap * 2);
        pane.addView(notice, np);
        notice.setVisibility(View.GONE);
        hideNotice = () -> notice.animate().alpha(0).setDuration(200).withEndAction(() -> notice.setVisibility(View.GONE));

        pane.setOnApplyWindowInsetsListener((v, insets) -> {
            if (fullscreen) {
                int top = insets.getSystemWindowInsetTop(), left = insets.getSystemWindowInsetLeft(),
                    right = insets.getSystemWindowInsetRight(), bottom = insets.getSystemWindowInsetBottom();
                if (Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
                    top = Math.max(top, insets.getDisplayCutout().getSafeInsetTop());
                    left = Math.max(left, insets.getDisplayCutout().getSafeInsetLeft());
                    right = Math.max(right, insets.getDisplayCutout().getSafeInsetRight());
                    bottom = Math.max(bottom, insets.getDisplayCutout().getSafeInsetBottom());
                }
                margins(toolbar, gap + left, gap + top, gap + right, gap);
                margins(toolsButton, gap + left, gap, gap + right, gap);
                margins(notice, gap * 2 + left, gap, gap * 2 + right, gap * 2 + bottom);
            }
            return insets;
        });
        outer.getViewTreeObserver().addOnGlobalLayoutListener(this::alignEmbedded);
    }

    static void margins(View v, int l, int t, int r, int b) {
        FrameLayout.LayoutParams p = (FrameLayout.LayoutParams)v.getLayoutParams();
        if (p.leftMargin == l && p.topMargin == t && p.rightMargin == r && p.bottomMargin == b) return;
        p.setMargins(l, t, r, b);
        v.setLayoutParams(p);
    }

    /** Icon over a short label, white on the dark video chrome. */
    static final class ToolButton extends LinearLayout {
        final Glyph glyph;
        final TextView label;
        final ImageView icon;
        ToolButton(Activity a, int kind, String text) {
            super(a);
            setOrientation(VERTICAL);
            setGravity(Gravity.CENTER);
            setPadding(Theme.dp(4), Theme.dp(6), Theme.dp(4), Theme.dp(5));
            glyph = new Glyph(kind, Color.WHITE, Theme.dp(22));
            icon = new ImageView(a);
            icon.setImageDrawable(glyph);
            addView(icon, Ui.lp(Theme.dp(22), Theme.dp(22)));
            label = Ui.text(a, text, 11, 0xd9ffffff);
            label.setSingleLine();
            label.setGravity(Gravity.CENTER_HORIZONTAL);
            label.setPadding(0, Theme.dp(3), 0, 0);
            // Explicit wrap width: a vertical LinearLayout would otherwise stretch the label and left-align it.
            addView(label, Ui.lp(-2, -2));
            setMinimumWidth(Theme.dp(56));
            setBackground(Ui.press(Ui.round(0, 16), 16));
            setFocusable(true);
            setContentDescription(text);
            Ui.role(this, Button.class);
        }
        void set(int kind, String text, boolean lit, boolean on) {
            Glyph g = new Glyph(kind, lit ? Theme.LAMP : Color.WHITE, Theme.dp(22));
            icon.setImageDrawable(g);
            label.setText(text);
            label.setTextColor(lit ? Theme.LAMP : 0xd9ffffff);
            setActivated(on);
            setContentDescription(text);
        }
    }

    ToolButton add(int glyph, String text, String id, Runnable action) {
        ToolButton b = new ToolButton(activity, glyph, text);
        b.setOnClickListener(v -> action.run());
        Ui.id(b, id);
        toolRow.addView(b, Ui.lp(-2, -2));
        return b;
    }
    void divider() {
        View line = new View(activity);
        line.setBackgroundColor(Theme.VIDEO_CHROME_LINE);
        LinearLayout.LayoutParams p = Ui.lp(Math.max(1, Theme.dp(1)), Theme.dp(28));
        p.setMargins(Theme.dp(4), 0, Theme.dp(4), 0);
        toolRow.addView(line, p);
    }

    void showToolbar(boolean show) {
        toolbar.setVisibility(show ? View.VISIBLE : View.GONE);
        toolsButton.setVisibility(fullscreen && !show ? View.VISIBLE : View.GONE);
    }

    /** Control state: 0 off, 1 requesting, 2 on. */
    void setControl(int state) {
        controlButton.set(Glyph.POINTER, state == 2 ? S.FULL_CONTROL_ON : state == 1 ? S.CONTROL_PENDING : S.TILE_CONTROL, state == 2, state == 2);
    }
    /** Right-click mode: the next tap becomes a right click (one shot). */
    void setRightArmed(boolean armed) {
        rightButton.set(Glyph.MOUSE_RIGHT, armed ? S.FULL_RIGHT_ARMED : S.FULL_RIGHT, armed, armed);
    }
    void setAudio(int state) {
        boolean on = state == MainActivity.AUDIO_ON;
        String text = on ? S.SOUND_ON_SHORT : state == MainActivity.AUDIO_CONNECTING || state == MainActivity.AUDIO_RECONNECTING ? S.SOUND_CONNECTING : S.TILE_SOUND;
        audioButton.set(on || state != MainActivity.AUDIO_OFF ? Glyph.SPEAKER : Glyph.SPEAKER_OFF, text, on, on);
    }

    void notice(String text) {
        if (!fullscreen) return;
        notice.setText(text);
        notice.removeCallbacks(hideNotice);
        notice.animate().cancel();
        notice.setAlpha(1);
        notice.setVisibility(View.VISIBLE);
        notice.postDelayed(hideNotice, Math.min(6000, 2800 + text.length() * 40L));
    }

    void alignEmbedded() {
        if (fullscreen || !visible || !onDesktopPage || anchor.getWidth() == 0)
            return;
        Rect rect = new Rect(0, 0, anchor.getWidth(), anchor.getHeight());
        outer.offsetDescendantRectToMyCoords(anchor, rect);
        FrameLayout.LayoutParams current = (FrameLayout.LayoutParams)pane.getLayoutParams();
        if (current.width != rect.width() || current.height != rect.height() ||
            current.leftMargin != rect.left || current.topMargin != rect.top) {
            FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(rect.width(), rect.height());
            p.leftMargin = rect.left;
            p.topMargin = rect.top;
            pane.setLayoutParams(p);
        }
    }
    void setVisible(boolean value) {
        if (!value && fullscreen)
            setFullscreen(false);
        visible = value;
        anchor.setVisibility(value ? View.VISIBLE : View.GONE);
        updatePagePresentation();
        pane.setKeepScreenOn(value);
        alignEmbedded();
    }
    void pageVisible(boolean value){onDesktopPage=value;updatePagePresentation();alignEmbedded();}
    void updatePagePresentation(){pane.setVisibility(visible?View.VISIBLE:View.GONE);boolean show=fullscreen||onDesktopPage;pane.setAlpha(show?1f:0f);pane.setTranslationX(show?0:Math.max(outer.getWidth(),1)+gap*2);}
    void setFullscreen(boolean value) {
        if (value == fullscreen || (value && !visible))
            return;
        fullscreen = value;
        if (value) {
            previousOrientation = activity.getRequestedOrientation();
            View focused = activity.getCurrentFocus();
            if (focused != null) {
                ((InputMethodManager)activity.getSystemService(Activity.INPUT_METHOD_SERVICE))
                    .hideSoftInputFromWindow(focused.getWindowToken(), 0);
                focused.clearFocus();
            }
            page.setVisibility(View.GONE);
            pane.setLayoutParams(new FrameLayout.LayoutParams(-1, -1));
            showToolbar(true);
        } else {
            page.setVisibility(View.VISIBLE);
            showToolbar(false);
            notice.setVisibility(View.GONE);
            activity.setRequestedOrientation(previousOrientation);
            alignEmbedded();
        }
        pane.invalidateOutline();
        updatePagePresentation();
        fullButton.setVisibility(value ? View.GONE : View.VISIBLE);
        systemBars();
        pane.requestApplyInsets();
    }
    void systemBars() {
        Window w = activity.getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = w.getInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                int light = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                controller.setSystemBarsAppearance(fullscreen || Theme.dark ? 0 : light, light);
                if (fullscreen)
                    controller.hide(WindowInsets.Type.systemBars());
                else
                    controller.show(WindowInsets.Type.systemBars());
            }
        }
        w.getDecorView().setSystemUiVisibility(
            fullscreen ? View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                             View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN |
                             View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                       : Theme.dark ? 0 : View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
    }
}
