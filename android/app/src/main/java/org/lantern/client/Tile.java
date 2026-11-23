package org.lantern.client;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.animation.DecelerateInterpolator;
import android.view.animation.OvershootInterpolator;
import android.widget.*;

/**
 * A switch tile for one independent desktop capability. "On" glows with a warm lantern rim and
 * amber icon beacon, "pending" shows a soft amber border, so on/off is unmistakable yet calm.
 */
final class Tile extends LinearLayout {
    static final int OFF = 0, PENDING = 1, ON = 2;
    final ImageView icon;
    final TextView title, state;
    Glyph glyph;
    final android.view.View dot;
    int mode = -1;
    private float glowProgress;
    private ValueAnimator glowAnim;

    Tile(Context c, int glyphKind, String label, Runnable action) {
        super(c);
        setOrientation(VERTICAL);
        setPadding(Theme.dp(14), Theme.dp(12), Theme.dp(14), Theme.dp(12));
        setMinimumHeight(Theme.dp(84));
        LinearLayout top = Ui.row(c);
        glyph = new Glyph(glyphKind, Theme.INK, Theme.dp(19));
        icon = new ImageView(c);
        icon.setScaleType(ImageView.ScaleType.CENTER);
        icon.setImageDrawable(glyph);
        top.addView(icon, Ui.lp(Theme.dp(36), Theme.dp(36)));
        top.addView(Ui.space(c), Ui.weight(1));
        dot = new android.view.View(c);
        top.addView(dot, Ui.lp(Theme.dp(8.5f), Theme.dp(8.5f)));
        addView(top, Ui.lp(-1, -2));
        title = Ui.medium(c, label, 14.5f, Theme.INK);
        Ui.oneLine(title);
        title.setPadding(0, Theme.dp(9), 0, 0);
        addView(title);
        state = Ui.text(c, "", 12, Theme.MUTED);
        Ui.oneLine(state);
        state.setPadding(0, Theme.dp(1.5f), 0, 0);
        state.setEllipsize(TextUtils.TruncateAt.END);
        addView(state);
        setGravity(Gravity.START);
        setClickable(true);
        setFocusable(true);
        Ui.role(this, android.widget.Switch.class);
        Ui.bounce(this, 0.97f);
        setOnClickListener(v -> action.run());
        set(OFF, "");
    }

    private void applySurface(float glow, boolean pending) {
        int topFill = Theme.blend(Theme.SURFACE, Theme.TILE_ON, glow);
        int botFill = Theme.blend(Theme.SURFACE, Theme.TILE_ON_GRAD, glow);
        int stroke = pending ? Theme.LAMP_GLOW : Theme.blend(Theme.RIM, Theme.LAMP_GLOW, glow);
        float strokeWidth = (glow > 0.1f || pending) ? 1.25f : 1f;
        GradientDrawable bg = Ui.gradient(topFill, botFill, stroke, strokeWidth, 18);
        setBackground(Ui.press(bg, 18));
        setElevation(Theme.dp(glow > 0.5f ? 3f : 1f));
        int badgeFill = pending ? Theme.LAMP_SOFT : Theme.blend(Theme.INSET, Theme.LAMP_SOFT, glow);
        int badgeStroke = (glow > 0.1f || pending) ? Theme.alpha(Theme.LAMP, 0.35f * Math.max(glow, pending ? 0.8f : 0f)) : 0;
        icon.setBackground(badgeStroke != 0 ? Ui.outline(badgeFill, badgeStroke, 1f, 12) : Ui.round(badgeFill, 12));
        int activeIcon = Theme.dark ? Theme.LAMP : Theme.LAMP_TEXT;
        glyph.setColor(pending ? Theme.LAMP_TEXT : Theme.blend(Theme.INK, activeIcon, glow));
    }

    void set(int value, String detail) {
        state.setText(detail);
        setContentDescription(title.getText() + "，" + detail);
        if (value == mode) return;
        boolean initial = mode < 0;
        mode = value;
        boolean on = value == ON;
        boolean pending = value == PENDING;
        setActivated(on);
        title.setTextColor(Theme.INK);
        state.setTextColor(on || pending ? Theme.LAMP_TEXT : Theme.MUTED);
        dot.setBackground(Ui.round(on ? Theme.LAMP : pending ? Theme.LAMP_SOFT : Theme.alpha(Theme.FAINT, 0.28f), 5));
        float targetGlow = on ? 1f : 0f;
        if (glowAnim != null) glowAnim.cancel();
        if (initial) {
            glowProgress = targetGlow;
            applySurface(glowProgress, pending);
        } else {
            float start = glowProgress;
            glowAnim = ValueAnimator.ofFloat(start, targetGlow);
            glowAnim.setDuration(200);
            glowAnim.setInterpolator(new DecelerateInterpolator(1.8f));
            glowAnim.addUpdateListener(anim -> {
                glowProgress = (Float)anim.getAnimatedValue();
                applySurface(glowProgress, mode == PENDING);
            });
            glowAnim.start();
            dot.animate().cancel();
            dot.setScaleX(0.65f);
            dot.setScaleY(0.65f);
            dot.animate().scaleX(1f).scaleY(1f).setDuration(220)
                .setInterpolator(new OvershootInterpolator(2.0f)).start();
        }
    }

    void setGlyph(int kind) {
        int activeIcon = Theme.dark ? Theme.LAMP : Theme.LAMP_TEXT;
        int color = mode == ON ? activeIcon : mode == PENDING ? Theme.LAMP_TEXT : Theme.INK;
        glyph = new Glyph(kind, color, Theme.dp(20));
        icon.setImageDrawable(glyph);
    }
}
