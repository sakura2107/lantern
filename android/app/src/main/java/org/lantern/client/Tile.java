package org.lantern.client;

import android.content.Context;
import android.text.TextUtils;
import android.view.Gravity;
import android.widget.*;

/**
 * A switch tile for one independent desktop capability. "On" is ink-filled with a lit lamp dot,
 * "pending" keeps the idle surface but states what is happening, so on/off is never ambiguous.
 */
final class Tile extends LinearLayout {
    static final int OFF = 0, PENDING = 1, ON = 2;
    final ImageView icon;
    final TextView title, state;
    Glyph glyph;
    final android.view.View dot;
    int mode = -1;

    Tile(Context c, int glyphKind, String label, Runnable action) {
        super(c);
        setOrientation(VERTICAL);
        setPadding(Theme.dp(14), Theme.dp(12), Theme.dp(14), Theme.dp(12));
        setMinimumHeight(Theme.dp(84));
        LinearLayout top = Ui.row(c);
        glyph = new Glyph(glyphKind, Theme.INK);
        icon = new ImageView(c);
        icon.setImageDrawable(glyph);
        top.addView(icon, Ui.lp(Theme.dp(24), Theme.dp(24)));
        top.addView(Ui.space(c), Ui.weight(1));
        dot = new android.view.View(c);
        top.addView(dot, Ui.lp(Theme.dp(8), Theme.dp(8)));
        addView(top, Ui.lp(-1, -2));
        title = Ui.medium(c, label, 15, Theme.INK);
        Ui.oneLine(title);
        title.setPadding(0, Theme.dp(10), 0, 0);
        addView(title);
        state = Ui.text(c, "", 13, Theme.MUTED);
        Ui.oneLine(state);
        state.setEllipsize(TextUtils.TruncateAt.END);
        addView(state);
        setGravity(Gravity.START);
        setClickable(true);
        setFocusable(true);
        Ui.role(this, android.widget.Switch.class);
        setOnClickListener(v -> action.run());
        set(OFF, "");
    }

    void set(int value, String detail) {
        state.setText(detail);
        setContentDescription(title.getText() + "，" + detail);
        if (value == mode) return;
        mode = value;
        setActivated(value == ON);
        boolean on = value == ON;
        int fill = on ? Theme.INK : Theme.SURFACE;
        setBackground(Ui.press(on ? Ui.round(fill, 18) : Ui.outline(fill, Theme.LINE, 18), 18));
        glyph.setColor(on ? Theme.ON_INK : Theme.INK);
        title.setTextColor(on ? Theme.ON_INK : Theme.INK);
        state.setTextColor(on ? Theme.alpha(Theme.ON_INK, 0.72f) : value == PENDING ? Theme.LAMP_TEXT : Theme.MUTED);
        dot.setBackground(Ui.round(on ? Theme.LAMP : value == PENDING ? Theme.LAMP_SOFT : 0, 4));
    }

    void setGlyph(int kind) {
        glyph = new Glyph(kind, mode == ON ? Theme.ON_INK : Theme.INK);
        icon.setImageDrawable(glyph);
    }
}
