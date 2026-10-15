package org.lantern.client;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.*;
import android.graphics.drawable.*;
import android.text.InputType;
import android.text.TextUtils;
import android.view.*;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.*;
import java.util.Calendar;
import java.util.Locale;

/** Small presentation toolkit: shapes, text, buttons, fields and stable test identifiers. */
final class Ui {
    static final int PRIMARY = 0, SECONDARY = 1, GHOST = 2, DANGER = 3;
    /** View tag key carrying a stable, non-localized identifier for automation. */
    static final int ID_KEY = 0x7f4c0001;

    private Ui() {}

    // ---------- identifiers ----------
    static <T extends View> T id(T view, String id) { view.setTag(ID_KEY, id); return view; }
    static String idOf(View view) { Object value = view.getTag(ID_KEY); return value instanceof String ? (String)value : null; }
    /** First shown view carrying the identifier, depth first. */
    static View find(View root, String id) {
        if (root == null) return null;
        if (id.equals(idOf(root)) && root.isShown()) return root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)root;
            for (int i = 0; i < group.getChildCount(); i++) {
                View found = find(group.getChildAt(i), id);
                if (found != null) return found;
            }
        }
        return null;
    }

    // ---------- formatting ----------
    static String extension(String name) {
        int slash = name.lastIndexOf('/'), dot = name.lastIndexOf('.');
        return dot <= slash + 1 ? "" : name.substring(dot + 1).toLowerCase(Locale.ROOT);
    }
    static String baseName(String path) { return path.substring(path.lastIndexOf('/') + 1); }
    static String size(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format(Locale.ROOT, "%.1f KB", bytes / 1024.0);
        if (bytes < 1024L * 1024 * 1024) return String.format(Locale.ROOT, "%.1f MB", bytes / 1048576.0);
        return String.format(Locale.ROOT, "%.2f GB", bytes / 1073741824.0);
    }
    static String rate(double bytesPerSecond) { return size(Math.round(bytesPerSecond)) + "/s"; }
    static String limit(long bytesPerSecond) {
        long mib = bytesPerSecond / (1024 * 1024);
        return mib > 0 ? mib + " MiB/s" : bytesPerSecond / 1024 + " KiB/s";
    }
    static String duration(long seconds) {
        if (seconds < 60) return seconds + " 秒";
        if (seconds < 3600) return (seconds / 60) + " 分 " + (seconds % 60) + " 秒";
        return (seconds / 3600) + " 小时 " + (seconds % 3600 / 60) + " 分";
    }
    /** Host timestamps are Windows FILETIME (100 ns since 1601). */
    static long fileTimeMillis(long modified) { return modified / 10000L - 11644473600000L; }
    static String shortDate(long modified) {
        if (modified <= 0) return "";
        Calendar then = Calendar.getInstance(), now = Calendar.getInstance();
        then.setTimeInMillis(fileTimeMillis(modified));
        if (then.get(Calendar.YEAR) == now.get(Calendar.YEAR)) {
            if (then.get(Calendar.DAY_OF_YEAR) == now.get(Calendar.DAY_OF_YEAR))
                return String.format(Locale.ROOT, S.TODAY_AT, then.get(Calendar.HOUR_OF_DAY), then.get(Calendar.MINUTE));
            return String.format(Locale.ROOT, S.MONTH_DAY, then.get(Calendar.MONTH) + 1, then.get(Calendar.DAY_OF_MONTH));
        }
        return String.format(Locale.ROOT, "%d/%d/%d", then.get(Calendar.YEAR), then.get(Calendar.MONTH) + 1, then.get(Calendar.DAY_OF_MONTH));
    }
    static String fullDate(long modified) {
        if (modified <= 0) return "";
        return java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.MEDIUM, java.text.DateFormat.SHORT)
            .format(new java.util.Date(fileTimeMillis(modified)));
    }
    /** Fingerprints are compared by eye, so group them in fours. */
    static String groupHex(String hex) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < hex.length(); i++) {
            if (i > 0 && i % 4 == 0) out.append(i % 32 == 0 ? '\n' : ' ');
            out.append(hex.charAt(i));
        }
        return out.toString();
    }
    static String shortHex(String hex) {
        return hex.length() <= 12 ? hex : hex.substring(0, 6) + "…" + hex.substring(hex.length() - 6);
    }

    // ---------- shapes ----------
    static GradientDrawable round(int fill, float radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(Theme.dp(radiusDp));
        return d;
    }
    static GradientDrawable outline(int fill, int stroke, float radiusDp) {
        GradientDrawable d = round(fill, radiusDp);
        d.setStroke(Math.max(1, Theme.dp(1)), stroke);
        return d;
    }
    static GradientDrawable topRound(int fill, float radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        float r = Theme.dp(radiusDp);
        d.setCornerRadii(new float[] {r, r, r, r, 0, 0, 0, 0});
        return d;
    }
    static Drawable press(Drawable content, float radiusDp) {
        return new RippleDrawable(ColorStateList.valueOf(Theme.alpha(Theme.INK, 0.12f)), content, round(0xffffffff, radiusDp));
    }
    static Drawable pressOval(int fill) {
        ShapeDrawable mask = new ShapeDrawable(new android.graphics.drawable.shapes.OvalShape());
        GradientDrawable content = new GradientDrawable();
        content.setShape(GradientDrawable.OVAL);
        content.setColor(fill);
        return new RippleDrawable(ColorStateList.valueOf(Theme.alpha(Theme.INK, 0.14f)), content, mask);
    }

    // ---------- layout ----------
    static LinearLayout column(Context c) { LinearLayout l = new LinearLayout(c); l.setOrientation(LinearLayout.VERTICAL); return l; }
    static LinearLayout row(Context c) { LinearLayout l = new LinearLayout(c); l.setGravity(Gravity.CENTER_VERTICAL); return l; }
    static LinearLayout.LayoutParams lp(int w, int h) { return new LinearLayout.LayoutParams(w, h); }
    static LinearLayout.LayoutParams weight(float weight) { return new LinearLayout.LayoutParams(0, -2, weight); }
    static LinearLayout.LayoutParams margins(LinearLayout.LayoutParams p, float l, float t, float r, float b) {
        p.setMargins(Theme.dp(l), Theme.dp(t), Theme.dp(r), Theme.dp(b));
        return p;
    }
    static View space(Context c) { return new View(c); }
    static View hairline(Context c) { View v = new View(c); v.setBackgroundColor(Theme.LINE); return v; }

    // ---------- text ----------
    static TextView text(Context c, CharSequence value, float sp, int color) {
        TextView t = new TextView(c);
        t.setText(value);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setTypeface(Theme.REGULAR);
        t.setIncludeFontPadding(true);
        return t;
    }
    static TextView medium(Context c, CharSequence value, float sp, int color) {
        TextView t = text(c, value, sp, color);
        t.setTypeface(Theme.MEDIUM);
        return t;
    }
    static TextView heading(Context c, CharSequence value) {
        TextView t = medium(c, value, 15, Theme.INK);
        t.setPadding(Theme.dp(4), Theme.dp(24), 0, Theme.dp(10));
        return t;
    }
    static TextView label(Context c, CharSequence value) {
        TextView t = medium(c, value, 13, Theme.MUTED);
        t.setPadding(0, Theme.dp(14), 0, Theme.dp(6));
        return t;
    }
    static TextView oneLine(TextView t) { t.setSingleLine(); t.setEllipsize(TextUtils.TruncateAt.END); return t; }
    static TextView numeric(TextView t) { t.setFontFeatureSettings("tnum"); return t; }

    // ---------- interactive ----------
    static void role(View v, Class<?> kind) {
        v.setAccessibilityDelegate(new View.AccessibilityDelegate() {
            @Override public void onInitializeAccessibilityNodeInfo(View host, AccessibilityNodeInfo info) {
                super.onInitializeAccessibilityNodeInfo(host, info);
                info.setClassName(kind.getName());
            }
        });
    }
    static Button button(Context c, String label, int style, Runnable click) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(15);
        b.setTypeface(Theme.MEDIUM);
        b.setStateListAnimator(null);
        b.setMinWidth(0); b.setMinimumWidth(0); b.setMinHeight(0); b.setMinimumHeight(0);
        b.setPadding(Theme.dp(20), 0, Theme.dp(20), 0);
        b.setSingleLine();
        b.setEllipsize(TextUtils.TruncateAt.END);
        b.setCompoundDrawablePadding(Theme.dp(8));
        style(b, style);
        if (click != null) b.setOnClickListener(v -> click.run());
        return b;
    }
    static void style(Button b, int style) {
        int fill, text;
        switch (style) {
        case PRIMARY: fill = Theme.INK; text = Theme.ON_INK; break;
        case DANGER: fill = Theme.DANGER_SOFT; text = Theme.DANGER; break;
        case GHOST: fill = 0; text = Theme.INK; break;
        default: fill = Theme.INSET; text = Theme.INK;
        }
        b.setTextColor(new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}},
                                          new int[] {Theme.alpha(text, 0.4f), text}));
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(style == PRIMARY ? 0x33ffffff : Theme.alpha(Theme.INK, 0.12f)),
                                           round(fill, 24), round(0xffffffff, 24)));
    }
    static void withIcon(Button b, int glyph) {
        Glyph g = new Glyph(glyph, b.getCurrentTextColor(), Theme.dp(18));
        g.setBounds(0, 0, g.size, g.size);
        b.setCompoundDrawablesRelative(g, null, null, null);
    }
    /** 44dp circular icon button with a description for screen readers. */
    static ImageView iconButton(Context c, int glyph, String description, Runnable click) {
        ImageView v = new ImageView(c);
        v.setImageDrawable(new Glyph(glyph, Theme.INK));
        v.setScaleType(ImageView.ScaleType.CENTER);
        v.setBackground(pressOval(0));
        v.setContentDescription(description);
        v.setFocusable(true);
        v.setClickable(true);
        role(v, Button.class);
        if (click != null) v.setOnClickListener(x -> click.run());
        return v;
    }
    static ImageView glyphTile(Context c, int glyph, int color, int fill, float sizeDp, float radiusDp) {
        ImageView v = new ImageView(c);
        v.setImageDrawable(new Glyph(glyph, color));
        v.setScaleType(ImageView.ScaleType.CENTER);
        v.setBackground(round(fill, radiusDp));
        v.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        return v;
    }
    /** A rounded chip; tone is one of the theme soft/strong pairs. */
    static TextView chip(Context c, String label, int textColor, int fill) {
        TextView t = medium(c, label, 12, textColor);
        t.setPadding(Theme.dp(10), Theme.dp(4), Theme.dp(10), Theme.dp(4));
        t.setBackground(round(fill, 12));
        t.setSingleLine();
        return t;
    }
    static EditText field(Context c, String hint, boolean secret) {
        EditText e = new EditText(c);
        e.setHint(hint);
        e.setTextColor(Theme.INK);
        e.setHintTextColor(Theme.FAINT);
        e.setTextSize(16);
        e.setSingleLine();
        e.setInputType(secret ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_CLASS_TEXT);
        e.setPadding(Theme.dp(14), Theme.dp(12), Theme.dp(14), Theme.dp(12));
        StateListDrawable background = new StateListDrawable();
        GradientDrawable focused = round(Theme.SURFACE, 14);
        focused.setStroke(Theme.dp(1.5f), Theme.INK);
        background.addState(new int[] {android.R.attr.state_focused}, focused);
        background.addState(new int[] {}, round(Theme.INSET, 14));
        e.setBackground(background);
        e.setMinHeight(Theme.dp(48));
        e.setHighlightColor(Theme.LAMP_SOFT);
        if (android.os.Build.VERSION.SDK_INT >= 29) {
            GradientDrawable cursor = round(Theme.INK, 1);
            cursor.setSize(Theme.dp(2), -1);
            e.setTextCursorDrawable(cursor);
        }
        return e;
    }
    static ProgressBar spinner(Context c, float sizeDp) {
        ProgressBar p = new ProgressBar(c, null, android.R.attr.progressBarStyleSmall);
        p.setIndeterminateTintList(ColorStateList.valueOf(Theme.INK));
        p.setLayoutParams(new LinearLayout.LayoutParams(Theme.dp(sizeDp), Theme.dp(sizeDp)));
        return p;
    }

    /** Thin rounded progress line; indeterminate mode animates a short segment. */
    static final class Meter extends View {
        float value;
        boolean indeterminate;
        int color = Theme.INK, track = Theme.INSET;
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        long started;
        Meter(Context c) { super(c); setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO); }
        void set(float fraction, int tone) {
            float next = Math.max(0, Math.min(1, fraction));
            if (next != value || tone != color || indeterminate) { value = next; color = tone; indeterminate = false; invalidate(); }
        }
        void busy(boolean on) {
            if (indeterminate == on) return;
            indeterminate = on;
            started = System.nanoTime();
            invalidate();
        }
        @Override protected void onDraw(Canvas canvas) {
            float h = getHeight(), w = getWidth(), r = h / 2;
            paint.setColor(track);
            canvas.drawRoundRect(0, 0, w, h, r, r, paint);
            paint.setColor(color);
            if (indeterminate) {
                float t = ((System.nanoTime() - started) / 1e9f) % 1.4f / 1.4f;
                float seg = w * 0.3f, x = -seg + (w + seg) * t;
                canvas.save();
                canvas.clipRect(0, 0, w, h);
                canvas.drawRoundRect(x, 0, x + seg, h, r, r, paint);
                canvas.restore();
                postInvalidateOnAnimation();
            } else if (value > 0) {
                canvas.drawRoundRect(0, 0, Math.max(h, w * value), h, r, r, paint);
            }
        }
    }
}
