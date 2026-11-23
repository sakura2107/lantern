package org.lantern.client;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Outline;
import android.graphics.Rect;
import android.os.Build;
import android.view.*;
import android.view.animation.DecelerateInterpolator;
import android.view.animation.OvershootInterpolator;
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
    final LinearLayout link; final TextView linkText, linkAction; final View linkDot; Runnable linkHandler;
    final View fullButton, toolsButton;
    final ToolButton controlButton, audioButton, rightButton;
    boolean fullscreen, visible, onDesktopPage;
    int previousOrientation;
    final int gap;
    final Runnable hideNotice, dimOrb;
    int insetLeft, insetTop, insetRight, insetBottom;
    // Session memory for draggable overlays (normalized fractions survive orientation changes).
    boolean orbDockRight = true, orbPositioned;
    float orbYFraction = 0.5f;
    boolean draggingOrb;
    boolean toolbarPositioned;
    float toolbarXFraction = 0.5f, toolbarYFraction = 0f;

    DesktopLayout(Activity a, FrameLayout host, LinearLayout normal, FrameLayout preview, View image, Actions actions) {
        activity = a;
        outer = host;
        page = normal;
        gap = Theme.dp(12);
        anchor = new FrameLayout(a);
        preview.addView(anchor, new FrameLayout.LayoutParams(-1, -1));
        anchor.setVisibility(View.GONE);
        pane = new FrameLayout(a);
        pane.setBackgroundColor(Color.BLACK);
        pane.setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View v, Outline outline) {
                outline.setRoundRect(0, 0, v.getWidth(), v.getHeight(), fullscreen ? 0 : Theme.dp(24));
            }
        });
        pane.setClipToOutline(true);
        pane.addView(image, new FrameLayout.LayoutParams(-1, -1));
        image.setVisibility(View.VISIBLE);
        pane.setVisibility(View.GONE);
        outer.addView(pane, new FrameLayout.LayoutParams(1, 1));

        // Fullscreen toolbar: frosted dark glass floating capsule with left drag handle, scrollable on narrow screens.
        toolbar = new FrameLayout(a) {
            @Override protected void onMeasure(int w, int h) {
                int availW = pane.getWidth() > 0 ? pane.getWidth() : MeasureSpec.getSize(w);
                int maxW = Math.max(Theme.dp(140), availW - safeLeft() - safeRight());
                int limitedW = MeasureSpec.makeMeasureSpec(maxW, MeasureSpec.AT_MOST);
                super.onMeasure(limitedW, h);
            }
        };
        toolbar.setBackground(Ui.glass(24));
        toolbar.setElevation(Theme.dp(10));
        toolbar.setPadding(Theme.dp(4), Theme.dp(4), Theme.dp(6), Theme.dp(4));

        LinearLayout toolbarContent = Ui.row(a);
        ImageView dragHandle = new ImageView(a);
        dragHandle.setImageDrawable(new Glyph(Glyph.GRIP, 0xb8ffffff, Theme.dp(18)));
        dragHandle.setScaleType(ImageView.ScaleType.CENTER);
        dragHandle.setBackground(Ui.pressGlass(Ui.round(0, 16), 16));
        dragHandle.setContentDescription(S.FULL_DRAG);
        dragHandle.setClickable(true);
        attachToolbarDrag(dragHandle);
        toolbarContent.addView(dragHandle, Ui.lp(Theme.dp(28), Theme.dp(46)));

        View handleSep = new View(a);
        handleSep.setBackgroundColor(Theme.VIDEO_CHROME_LINE);
        LinearLayout.LayoutParams hsp = Ui.lp(Math.max(1, Theme.dp(1)), Theme.dp(24));
        hsp.setMargins(Theme.dp(2), 0, Theme.dp(4), 0);
        toolbarContent.addView(handleSep, hsp);

        HorizontalScrollView scroll = new HorizontalScrollView(a);
        scroll.setHorizontalScrollBarEnabled(false);
        toolRow = Ui.row(a);
        scroll.addView(toolRow);
        toolbarContent.addView(scroll, Ui.lp(-2, -2));
        toolbar.addView(toolbarContent, new FrameLayout.LayoutParams(-2, -2));

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
        pane.addView(toolbar, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START));
        toolbar.setVisibility(View.GONE);

        ImageView full = new ImageView(a);
        full.setImageDrawable(new Glyph(Glyph.EXPAND, Color.WHITE, Theme.dp(19)));
        full.setScaleType(ImageView.ScaleType.CENTER);
        full.setBackground(Ui.pressGlass(Ui.glass(15), 15));
        full.setElevation(Theme.dp(6));
        full.setContentDescription(S.TOOL_FULLSCREEN);
        full.setOnClickListener(v -> setFullscreen(true));
        Ui.role(full, Button.class);
        Ui.bounce(full, 0.92f);
        Ui.id(full, "desktop.fullscreen");
        FrameLayout.LayoutParams corner = new FrameLayout.LayoutParams(Theme.dp(40), Theme.dp(40), Gravity.TOP | Gravity.END);
        corner.setMargins(gap, gap, gap, gap);
        pane.addView(full, corner);
        fullButton = full;

        // Collapsed floating orb: free draggable, magnetically snaps to left/right safe edge, dims to ~0.42 after 3s idle.
        ImageView handle = new ImageView(a);
        handle.setImageDrawable(new Glyph(Glyph.TOOLS, Color.WHITE, Theme.dp(21)));
        handle.setScaleType(ImageView.ScaleType.CENTER);
        handle.setBackground(Ui.pressGlass(Ui.outline(Theme.VIDEO_CHROME, Theme.VIDEO_CHROME_STROKE, 1.2f, 24), 24));
        handle.setElevation(Theme.dp(10));
        handle.setContentDescription(S.FULL_TOOLS);
        handle.setOnClickListener(v -> showToolbar(true));
        Ui.role(handle, Button.class);
        Ui.id(handle, "full.tools");
        pane.addView(handle, new FrameLayout.LayoutParams(Theme.dp(48), Theme.dp(48), Gravity.TOP | Gravity.START));
        handle.setVisibility(View.GONE);
        toolsButton = handle;
        dimOrb = () -> {
            if (fullscreen && toolsButton.getVisibility() == View.VISIBLE && !draggingOrb)
                toolsButton.animate().alpha(0.42f).setDuration(350).start();
        };
        attachOrbDrag(handle);

        notice = Ui.text(a, "", 13, Color.WHITE);
        notice.setMaxLines(2);
        notice.setEllipsize(android.text.TextUtils.TruncateAt.END);
        notice.setBackground(Ui.glass(18));
        notice.setElevation(Theme.dp(8));
        notice.setPadding(Theme.dp(15), Theme.dp(9), Theme.dp(15), Theme.dp(9));
        notice.setMaxWidth(Theme.dp(520));
        notice.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        FrameLayout.LayoutParams np = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        np.setMargins(gap * 2, gap, gap * 2, gap * 2);
        pane.addView(notice, np);
        notice.setVisibility(View.GONE);
        hideNotice = () -> notice.animate().alpha(0f).translationY(Theme.dp(8)).setDuration(180)
            .withEndAction(() -> notice.setVisibility(View.GONE)).start();

        // Persistent link banner, top centre, frosted dark glass capsule with amber status accent.
        link = Ui.row(a);
        link.setGravity(Gravity.CENTER_VERTICAL);
        link.setBackground(Ui.glass(Theme.VIDEO_CHROME, Theme.alpha(Theme.LAMP, 0.42f), 50));
        link.setElevation(Theme.dp(8));
        link.setPadding(Theme.dp(13), Theme.dp(6), Theme.dp(6), Theme.dp(6));
        link.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        linkDot = new View(a);
        linkDot.setBackground(Ui.round(Theme.LAMP, 4));
        LinearLayout.LayoutParams ldp = Ui.lp(Theme.dp(7), Theme.dp(7));
        ldp.setMarginEnd(Theme.dp(8));
        link.addView(linkDot, ldp);
        linkText = Ui.medium(a, "", 12.5f, Color.WHITE);
        linkText.setSingleLine();
        linkText.setPadding(0, Theme.dp(2), Theme.dp(8), Theme.dp(2));
        link.addView(linkText, Ui.lp(-2, -2));
        linkAction = Ui.medium(a, S.LINK_RECONNECT, 12.5f, Theme.LAMP);
        linkAction.setPadding(Theme.dp(12), Theme.dp(5), Theme.dp(12), Theme.dp(5));
        linkAction.setBackground(Ui.pressGlass(Ui.outline(Theme.VIDEO_CHROME_ACTIVE, Theme.alpha(Theme.LAMP, 0.45f), 1f, 50), 50));
        linkAction.setOnClickListener(v -> { if (linkHandler != null) linkHandler.run(); });
        Ui.role(linkAction, Button.class);
        Ui.bounce(linkAction, 0.95f);
        Ui.id(linkAction, "desktop.reconnect");
        link.addView(linkAction, Ui.lp(-2, -2));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        lp.setMargins(gap, gap, gap, gap);
        pane.addView(link, lp);
        link.setVisibility(View.GONE);

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
                insetLeft = left; insetTop = top; insetRight = right; insetBottom = bottom;
                margins(notice, gap * 2 + left, gap, gap * 2 + right, gap * 2 + bottom);
                toolbar.requestLayout();
                pane.post(() -> {
                    clampOverlayPositions(false);
                    placeLink();
                });
            }
            return insets;
        });
        pane.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (fullscreen && ((r - l) != (or - ol) || (b - t) != (ob - ot))) {
                pane.post(() -> {
                    clampOverlayPositions(false);
                    placeLink();
                });
            }
        });
        toolbar.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (fullscreen && ((r - l) != (or - ol) || (b - t) != (ob - ot))) {
                clampToolbarPosition();
                placeLink();
            }
        });
        outer.getViewTreeObserver().addOnGlobalLayoutListener(this::alignEmbedded);
    }

    private int safeLeft() { return insetLeft + gap; }
    private int safeTop() { return insetTop + gap; }
    private int safeRight() { return insetRight + gap; }
    private int safeBottom() { return insetBottom + gap; }

    private void scheduleOrbDim() {
        toolsButton.removeCallbacks(dimOrb);
        if (fullscreen && toolsButton.getVisibility() == View.VISIBLE && !draggingOrb)
            toolsButton.postDelayed(dimOrb, 3000);
    }

    private void wakeOrb() {
        toolsButton.removeCallbacks(dimOrb);
        toolsButton.animate().cancel();
        toolsButton.setAlpha(1f);
    }

    private void attachOrbDrag(View orb) {
        final int slop = ViewConfiguration.get(activity).getScaledTouchSlop();
        orb.setOnTouchListener(new View.OnTouchListener() {
            float downRawX, downRawY, startTx, startTy;
            @Override public boolean onTouch(View v, MotionEvent e) {
                switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    draggingOrb = false;
                    wakeOrb();
                    downRawX = e.getRawX();
                    downRawY = e.getRawY();
                    startTx = v.getTranslationX();
                    startTy = v.getTranslationY();
                    v.animate().scaleX(0.93f).scaleY(0.93f).setDuration(80).start();
                    if (v.getParent() != null) v.getParent().requestDisallowInterceptTouchEvent(true);
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    float dx = e.getRawX() - downRawX, dy = e.getRawY() - downRawY;
                    if (!draggingOrb && Math.hypot(dx, dy) > slop) {
                        draggingOrb = true;
                        v.animate().scaleX(1.06f).scaleY(1.06f).setDuration(100).start();
                    }
                    if (draggingOrb) {
                        int pw = pane.getWidth(), ph = pane.getHeight();
                        int ow = v.getWidth() > 0 ? v.getWidth() : Theme.dp(48);
                        int oh = v.getHeight() > 0 ? v.getHeight() : Theme.dp(48);
                        float minX = safeLeft(), maxX = Math.max(minX, pw - safeRight() - ow);
                        float minY = safeTop(), maxY = Math.max(minY, ph - safeBottom() - oh);
                        v.setTranslationX(Math.max(minX, Math.min(maxX, startTx + dx)));
                        v.setTranslationY(Math.max(minY, Math.min(maxY, startTy + dy)));
                    }
                    return true;
                }
                case MotionEvent.ACTION_UP:
                    if (!draggingOrb) {
                        v.animate().scaleX(1f).scaleY(1f).setDuration(120).start();
                        v.performClick();
                    } else {
                        draggingOrb = false;
                        snapOrbToEdge(true);
                    }
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    draggingOrb = false;
                    snapOrbToEdge(true);
                    return true;
                default:
                    return false;
                }
            }
        });
    }

    private void snapOrbToEdge(boolean animate) {
        int pw = pane.getWidth(), ph = pane.getHeight();
        int ow = toolsButton.getWidth() > 0 ? toolsButton.getWidth() : Theme.dp(48);
        int oh = toolsButton.getHeight() > 0 ? toolsButton.getHeight() : Theme.dp(48);
        if (pw <= 0 || ph <= 0) return;
        float minX = safeLeft(), maxX = Math.max(minX, pw - safeRight() - ow);
        float minY = safeTop(), maxY = Math.max(minY, ph - safeBottom() - oh);
        float curX = toolsButton.getTranslationX(), curY = toolsButton.getTranslationY();
        if (orbPositioned || animate) {
            orbDockRight = (curX + ow * 0.5f) >= pw * 0.5f;
            float clampedY = Math.max(minY, Math.min(maxY, curY));
            orbYFraction = maxY > minY ? (clampedY - minY) / (maxY - minY) : 0.5f;
            orbPositioned = true;
        }
        float targetX = orbDockRight ? maxX : minX;
        float targetY = minY + Math.max(0f, Math.min(1f, orbYFraction)) * (maxY - minY);
        toolsButton.animate().cancel();
        if (animate) {
            toolsButton.animate().translationX(targetX).translationY(targetY)
                .scaleX(1f).scaleY(1f).alpha(1f).setDuration(280)
                .setInterpolator(new OvershootInterpolator(1.05f)).start();
        } else {
            toolsButton.setTranslationX(targetX);
            toolsButton.setTranslationY(targetY);
            toolsButton.setScaleX(1f);
            toolsButton.setScaleY(1f);
        }
        scheduleOrbDim();
    }

    private void attachToolbarDrag(View handle) {
        handle.setOnTouchListener(new View.OnTouchListener() {
            float downRawX, downRawY, startTx, startTy;
            @Override public boolean onTouch(View v, MotionEvent e) {
                switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downRawX = e.getRawX();
                    downRawY = e.getRawY();
                    startTx = toolbar.getTranslationX();
                    startTy = toolbar.getTranslationY();
                    toolbar.animate().cancel();
                    toolbar.animate().scaleX(1.015f).scaleY(1.015f).setDuration(90).start();
                    if (v.getParent() != null) v.getParent().requestDisallowInterceptTouchEvent(true);
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    int pw = pane.getWidth(), ph = pane.getHeight();
                    int tw = toolbar.getWidth(), th = toolbar.getHeight();
                    if (pw <= 0 || ph <= 0 || tw <= 0 || th <= 0) return true;
                    float minX = safeLeft(), maxX = Math.max(minX, pw - safeRight() - tw);
                    float minY = safeTop(), maxY = Math.max(minY, ph - safeBottom() - th);
                    float nx = Math.max(minX, Math.min(maxX, startTx + (e.getRawX() - downRawX)));
                    float ny = Math.max(minY, Math.min(maxY, startTy + (e.getRawY() - downRawY)));
                    toolbar.setTranslationX(nx);
                    toolbar.setTranslationY(ny);
                    toolbarPositioned = true;
                    toolbarXFraction = maxX > minX ? (nx - minX) / (maxX - minX) : 0.5f;
                    toolbarYFraction = maxY > minY ? (ny - minY) / (maxY - minY) : 0f;
                    if (link.getVisibility() == View.VISIBLE) placeLink();
                    return true;
                }
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    toolbar.animate().scaleX(1f).scaleY(1f).setDuration(150)
                        .setInterpolator(new DecelerateInterpolator(1.6f)).start();
                    clampToolbarPosition();
                    placeLink();
                    return true;
                default:
                    return false;
                }
            }
        });
    }

    private void clampToolbarPosition() {
        int pw = pane.getWidth(), ph = pane.getHeight();
        int tw = toolbar.getWidth(), th = toolbar.getHeight();
        if (pw <= 0 || ph <= 0 || tw <= 0 || th <= 0) return;
        float minX = safeLeft(), maxX = Math.max(minX, pw - safeRight() - tw);
        float minY = safeTop(), maxY = Math.max(minY, ph - safeBottom() - th);
        float x = toolbarPositioned ? minX + Math.max(0f, Math.min(1f, toolbarXFraction)) * (maxX - minX)
                                    : minX + (maxX - minX) * 0.5f;
        float y = toolbarPositioned ? minY + Math.max(0f, Math.min(1f, toolbarYFraction)) * (maxY - minY)
                                    : minY;
        toolbar.setTranslationX(Math.max(minX, Math.min(maxX, x)));
        toolbar.setTranslationY(Math.max(minY, Math.min(maxY, y)));
    }

    private void clampOverlayPositions(boolean animateOrb) {
        clampToolbarPosition();
        int pw = pane.getWidth(), ph = pane.getHeight();
        if (pw <= 0 || ph <= 0) return;
        int ow = toolsButton.getWidth() > 0 ? toolsButton.getWidth() : Theme.dp(48);
        int oh = toolsButton.getHeight() > 0 ? toolsButton.getHeight() : Theme.dp(48);
        float minX = safeLeft(), maxX = Math.max(minX, pw - safeRight() - ow);
        float minY = safeTop(), maxY = Math.max(minY, ph - safeBottom() - oh);
        float targetX = orbDockRight ? maxX : minX;
        float targetY = minY + Math.max(0f, Math.min(1f, orbYFraction)) * (maxY - minY);
        if (animateOrb) {
            toolsButton.animate().translationX(targetX).translationY(targetY).setDuration(240).start();
        } else {
            toolsButton.setTranslationX(targetX);
            toolsButton.setTranslationY(targetY);
        }
    }

    static void margins(View v, int l, int t, int r, int b) {
        FrameLayout.LayoutParams p = (FrameLayout.LayoutParams)v.getLayoutParams();
        if (p.leftMargin == l && p.topMargin == t && p.rightMargin == r && p.bottomMargin == b) return;
        p.setMargins(l, t, r, b);
        v.setLayoutParams(p);
    }

    /** Icon over a short label, white on the frosted dark video chrome, warm amber when active. */
    static final class ToolButton extends LinearLayout {
        final Glyph glyph;
        final TextView label;
        final ImageView icon;
        ToolButton(Activity a, int kind, String text) {
            super(a);
            setOrientation(VERTICAL);
            setGravity(Gravity.CENTER);
            setPadding(Theme.dp(5), Theme.dp(6), Theme.dp(5), Theme.dp(5));
            glyph = new Glyph(kind, Color.WHITE, Theme.dp(20));
            icon = new ImageView(a);
            icon.setImageDrawable(glyph);
            addView(icon, Ui.lp(Theme.dp(20), Theme.dp(20)));
            label = Ui.medium(a, text, 10.5f, 0xe0ffffff);
            label.setSingleLine();
            label.setGravity(Gravity.CENTER_HORIZONTAL);
            label.setPadding(0, Theme.dp(3), 0, 0);
            // Explicit wrap width: a vertical LinearLayout would otherwise stretch the label and left-align it.
            addView(label, Ui.lp(-2, -2));
            setMinimumWidth(Theme.dp(54));
            setBackground(Ui.pressGlass(Ui.round(0, 16), 16));
            setFocusable(true);
            setContentDescription(text);
            Ui.role(this, Button.class);
            Ui.bounce(this, 0.94f);
        }
        void set(int kind, String text, boolean lit, boolean on) {
            Glyph g = new Glyph(kind, lit ? Theme.LAMP : Color.WHITE, Theme.dp(20));
            icon.setImageDrawable(g);
            label.setText(text);
            label.setTextColor(lit ? Theme.LAMP : 0xe0ffffff);
            setBackground(lit
                ? Ui.pressGlass(Ui.outline(Theme.VIDEO_CHROME_ACTIVE, Theme.alpha(Theme.LAMP, 0.45f), 1f, 16), 16)
                : Ui.pressGlass(Ui.round(0, 16), 16));
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
        LinearLayout.LayoutParams p = Ui.lp(Math.max(1, Theme.dp(1)), Theme.dp(26));
        p.setMargins(Theme.dp(4), 0, Theme.dp(4), 0);
        toolRow.addView(line, p);
    }

    void showToolbar(boolean show) {
        toolsButton.removeCallbacks(dimOrb);
        toolbar.animate().cancel();
        toolsButton.animate().cancel();
        toolbar.setVisibility(show ? View.VISIBLE : View.GONE);
        toolsButton.setVisibility(fullscreen && !show ? View.VISIBLE : View.GONE);
        if (show) {
            toolbar.setAlpha(0f);
            toolbar.setScaleX(0.90f);
            toolbar.setScaleY(0.90f);
            toolbar.post(() -> {
                if (toolbar.getVisibility() != View.VISIBLE) return;
                clampToolbarPosition();
                placeLink();
                toolbar.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(180)
                    .setInterpolator(new DecelerateInterpolator(2.0f)).start();
            });
        } else if (fullscreen) {
            toolsButton.setAlpha(0f);
            toolsButton.setScaleX(0.76f);
            toolsButton.setScaleY(0.76f);
            toolsButton.post(() -> {
                if (!fullscreen || toolsButton.getVisibility() != View.VISIBLE) return;
                clampOverlayPositions(false);
                placeLink();
                toolsButton.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(210)
                    .setInterpolator(new OvershootInterpolator(1.3f)).start();
                scheduleOrbDim();
            });
        } else {
            toolbar.post(this::placeLink);
        }
    }

    /** Below the fullscreen toolbar when it sits near the top; in the page card, below the stats line and full-screen button. */
    void placeLink() {
        int top = fullscreen ? safeTop() : gap * 2 + Theme.dp(40);
        if (fullscreen && toolbar.getVisibility() == View.VISIBLE && toolbar.getHeight() > 0) {
            float barTop = toolbar.getTranslationY();
            if (barTop < safeTop() + Theme.dp(72))
                top = Math.round(barTop + toolbar.getHeight()) + Theme.dp(8);
        }
        margins(link, gap, top, gap, gap);
    }
    /** text null hides the banner; label/action null hides the button. */
    void link(String text, String label, Runnable action) {
        if (text == null) { link.setVisibility(View.GONE); linkHandler = null; return; }
        if (!text.contentEquals(linkText.getText())) linkText.setText(text);
        if (label != null && !label.contentEquals(linkAction.getText())) { linkAction.setText(label); linkAction.setContentDescription(label); }
        if (label == null) action = null;
        linkHandler = action;
        linkAction.setVisibility(action == null ? View.GONE : View.VISIBLE);
        linkDot.setBackground(Ui.round(action != null ? Theme.DANGER : Theme.LAMP, 4));
        link.setBackground(Ui.glass(Theme.VIDEO_CHROME, Theme.alpha(action != null ? Theme.DANGER : Theme.LAMP, 0.45f), 50));
        link.setPadding(Theme.dp(13), Theme.dp(6), Theme.dp(action == null ? 14 : 6), Theme.dp(6));
        if (link.getVisibility() != View.VISIBLE) { link.setVisibility(View.VISIBLE); placeLink(); }
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
        if (fullscreen) inputNotice(text);
    }
    void inputNotice(String text) {
        if (!visible || (!fullscreen && !onDesktopPage)) return;
        notice.setText(text);
        notice.removeCallbacks(hideNotice);
        notice.animate().cancel();
        boolean wasHidden = notice.getVisibility() != View.VISIBLE;
        notice.setVisibility(View.VISIBLE);
        if (wasHidden) {
            notice.setAlpha(0f);
            notice.setTranslationY(Theme.dp(10));
            notice.animate().alpha(1f).translationY(0f).setDuration(180)
                .setInterpolator(new DecelerateInterpolator(1.8f)).start();
        } else {
            notice.setAlpha(1f);
            notice.setTranslationY(0f);
        }
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
            toolsButton.removeCallbacks(dimOrb);
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
