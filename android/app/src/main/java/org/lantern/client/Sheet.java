package org.lantern.client;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.text.TextUtils;
import android.view.*;
import android.view.animation.PathInterpolator;
import android.view.inputmethod.EditorInfo;
import android.widget.*;

/**
 * Bottom sheet built on a plain Dialog: list actions, confirmations and text prompts share one
 * look. Back and outside taps dismiss; actions dismiss first, then run on the UI thread.
 */
final class Sheet {
    interface Text { void accept(String value); }

    final Activity activity;
    final Dialog dialog;
    final LinearLayout root, body, footer;
    Runnable dismissed;

    Sheet(Activity a, String title, CharSequence subtitle) {
        activity = a;
        dialog = new Dialog(a);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        root = Ui.column(a);
        root.setBackground(Ui.topRoundSurface(Theme.SURFACE, Theme.RIM, 28));
        root.setPadding(Theme.dp(10), Theme.dp(10), Theme.dp(10), Theme.dp(14));
        View handle = new View(a);
        handle.setBackground(Ui.round(Theme.alpha(Theme.MUTED, 0.36f), 4));
        LinearLayout.LayoutParams hp = Ui.lp(Theme.dp(40), Theme.dp(4));
        hp.gravity = Gravity.CENTER_HORIZONTAL;
        hp.topMargin = Theme.dp(2);
        hp.bottomMargin = Theme.dp(12);
        root.addView(handle, hp);
        if (title != null) {
            TextView t = Ui.medium(a, title, 18.5f, Theme.INK);
            t.setPadding(Theme.dp(14), Theme.dp(2), Theme.dp(14), subtitle == null ? Theme.dp(10) : Theme.dp(3));
            t.setMaxLines(2);
            t.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            root.addView(t);
        }
        if (subtitle != null) {
            TextView s = Ui.text(a, subtitle, 13.5f, Theme.MUTED);
            s.setPadding(Theme.dp(14), 0, Theme.dp(14), Theme.dp(12));
            s.setLineSpacing(0, 1.2f);
            root.addView(s);
        }
        ScrollView scroll = new ScrollView(a) {
            @Override protected void onMeasure(int w, int h) {
                int cap = (int)(getResources().getDisplayMetrics().heightPixels * 0.68f);
                super.onMeasure(w, MeasureSpec.makeMeasureSpec(cap, MeasureSpec.AT_MOST));
            }
        };
        scroll.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);
        body = Ui.column(a);
        scroll.addView(body);
        root.addView(scroll, Ui.lp(-1, -2));
        footer = Ui.row(a);
        footer.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        footer.setPadding(Theme.dp(10), Theme.dp(10), Theme.dp(10), 0);
        footer.setVisibility(View.GONE);
        root.addView(footer, Ui.lp(-1, -2));
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int bottom;
            if (Build.VERSION.SDK_INT >= 30)
                bottom = insets.getInsets(WindowInsets.Type.navigationBars() | WindowInsets.Type.ime()).bottom;
            else
                bottom = insets.getSystemWindowInsetBottom();
            v.setPadding(Theme.dp(10), Theme.dp(10), Theme.dp(10), Theme.dp(14) + bottom);
            return insets;
        });
        dialog.setContentView(root);
        dialog.setCanceledOnTouchOutside(true);
        dialog.setOnDismissListener(d -> { if (dismissed != null) dismissed.run(); });
        Window w = dialog.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(0));
            w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            w.setGravity(Gravity.BOTTOM);
            w.setWindowAnimations(0);
            w.setDimAmount(Theme.dark ? 0.68f : 0.40f);
            w.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        }
    }

    View row(int glyph, CharSequence title, CharSequence detail, int tint, View trailing) {
        LinearLayout r = Ui.row(activity);
        r.setPadding(Theme.dp(12), Theme.dp(11), Theme.dp(12), Theme.dp(11));
        r.setMinimumHeight(Theme.dp(56));
        r.setBackground(Ui.press(Ui.round(0, 16), 16));
        if (glyph >= 0) {
            int tileFill = tint == Theme.DANGER ? Theme.DANGER_SOFT : tint == Theme.FAINT ? 0 : Theme.INSET;
            ImageView icon = Ui.glyphTile(activity, glyph, tint, tileFill, 40, 12);
            r.addView(icon, Ui.lp(Theme.dp(40), Theme.dp(40)));
        }
        LinearLayout lines = Ui.column(activity);
        LinearLayout.LayoutParams p = Ui.weight(1);
        p.setMarginStart(Theme.dp(glyph >= 0 ? 12 : 4));
        r.addView(lines, p);
        TextView t = Ui.medium(activity, title, 15.5f, tint == Theme.DANGER ? Theme.DANGER : Theme.INK);
        t.setMaxLines(2);
        t.setEllipsize(TextUtils.TruncateAt.END);
        lines.addView(t);
        if (detail != null && detail.length() > 0) {
            TextView d = Ui.text(activity, detail, 13, Theme.MUTED);
            d.setPadding(0, Theme.dp(2), 0, 0);
            lines.addView(d);
        }
        if (trailing != null) r.addView(trailing);
        body.addView(r, Ui.lp(-1, -2));
        return r;
    }
    Sheet item(int glyph, CharSequence title, CharSequence detail, Runnable action) {
        View r = row(glyph, title, detail, Theme.INK, null);
        r.setOnClickListener(v -> { dialog.dismiss(); if (action != null) action.run(); });
        Ui.role(r, Button.class);
        return this;
    }
    Sheet danger(int glyph, CharSequence title, CharSequence detail, Runnable action) {
        View r = row(glyph, title, detail, Theme.DANGER, null);
        r.setOnClickListener(v -> { dialog.dismiss(); if (action != null) action.run(); });
        Ui.role(r, Button.class);
        return this;
    }
    /** A visible but inert row for capabilities this version does not provide. */
    Sheet unsupported(int glyph, CharSequence title, CharSequence detail) {
        TextView tag = Ui.chip(activity, S.UNSUPPORTED, Theme.MUTED, Theme.INSET);
        View r = row(glyph, title, detail, Theme.FAINT, tag);
        r.setBackground(null);
        r.setEnabled(false);
        r.setAlpha(0.75f);
        return this;
    }
    /** A single-choice option; the current one carries a warm lantern check and soft wash. */
    Sheet option(CharSequence title, CharSequence detail, boolean checked, Runnable action) {
        ImageView check = new ImageView(activity);
        check.setImageDrawable(new Glyph(Glyph.CHECK, Theme.dark ? Theme.LAMP : Theme.LAMP_TEXT));
        check.setVisibility(checked ? View.VISIBLE : View.INVISIBLE);
        View r = row(-1, title, detail, Theme.INK, check);
        if (checked) r.setBackground(Ui.press(Ui.outline(Theme.LAMP_SOFT, Theme.alpha(Theme.LAMP, 0.28f), 1f, 16), 16));
        r.setOnClickListener(v -> { dialog.dismiss(); if (action != null) action.run(); });
        r.setSelected(checked);
        Ui.role(r, RadioButton.class);
        return this;
    }
    Sheet section(String label) {
        TextView t = Ui.medium(activity, label, 12.5f, Theme.MUTED);
        t.setLetterSpacing(0.03f);
        t.setPadding(Theme.dp(14), Theme.dp(14), Theme.dp(14), Theme.dp(5));
        body.addView(t);
        return this;
    }
    Sheet view(View content) {
        body.addView(content, Ui.lp(-1, -2));
        return this;
    }
    Sheet buttons(String cancel, String confirm, int style, Runnable action) {
        footer.setVisibility(View.VISIBLE);
        if (cancel != null) {
            Button c = Ui.button(activity, cancel, Ui.GHOST, dialog::dismiss);
            footer.addView(c, Ui.lp(-2, Theme.dp(46)));
        }
        if (confirm != null) {
            Button ok = Ui.button(activity, confirm, style, () -> { dialog.dismiss(); if (action != null) action.run(); });
            LinearLayout.LayoutParams p = Ui.lp(-2, Theme.dp(46));
            p.setMarginStart(Theme.dp(8));
            footer.addView(ok, p);
        }
        return this;
    }
    Sheet onDismiss(Runnable r) { dismissed = r; return this; }
    static Sheet current; // the most recently shown sheet (tests dismiss it)
    Sheet show() {
        current = this;
        if (!activity.isFinishing() && !activity.isDestroyed()) {
            dialog.show();
            root.animate().cancel();
            root.setTranslationY(Theme.dp(56));
            root.setAlpha(0f);
            root.animate().translationY(0f).alpha(1f).setDuration(240)
                .setInterpolator(new PathInterpolator(0.2f, 0.92f, 0.25f, 1f)).start();
        }
        return this;
    }
    void dismiss() { if (dialog.isShowing()) dialog.dismiss(); }

    static TextView paragraph(Activity a, CharSequence text) {
        TextView t = Ui.text(a, text, 15, Theme.INK);
        t.setLineSpacing(0, 1.2f);
        t.setPadding(Theme.dp(16), Theme.dp(4), Theme.dp(16), Theme.dp(8));
        t.setTextIsSelectable(false);
        return t;
    }

    static Sheet confirm(Activity a, String title, CharSequence message, String confirmLabel, boolean danger, Runnable action) {
        return new Sheet(a, title, message).buttons(S.CANCEL, confirmLabel, danger ? Ui.DANGER : Ui.PRIMARY, action).show();
    }
    static Sheet info(Activity a, String title, CharSequence message) {
        Sheet s = new Sheet(a, title, null);
        s.view(paragraph(a, message));
        return s.buttons(null, S.DONE, Ui.SECONDARY, null).show();
    }
    static Sheet prompt(Activity a, String title, CharSequence message, String hint, String initial, String confirmLabel, Text action) {
        Sheet s = new Sheet(a, title, message);
        EditText field = Ui.field(a, hint, false);
        field.setText(initial);
        field.setSelectAllOnFocus(initial != null && !initial.isEmpty() && initial.indexOf('/') < 0);
        if (initial != null && initial.indexOf('/') >= 0) field.setSelection(initial.length());
        LinearLayout box = Ui.column(a);
        box.setPadding(Theme.dp(16), Theme.dp(4), Theme.dp(16), Theme.dp(8));
        box.addView(field, Ui.lp(-1, -2));
        s.view(box);
        Runnable submit = () -> {
            String value = field.getText().toString().trim();
            if (!value.isEmpty()) action.accept(value);
        };
        s.buttons(S.CANCEL, confirmLabel, Ui.PRIMARY, submit);
        field.setImeOptions(EditorInfo.IME_ACTION_DONE);
        field.setOnEditorActionListener((v, id, e) -> { s.dismiss(); submit.run(); return true; });
        Window w = s.dialog.getWindow();
        if (w != null) w.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        field.requestFocus();
        return s.show();
    }
}
