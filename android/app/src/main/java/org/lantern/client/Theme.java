package org.lantern.client;

import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Typeface;

/**
 * Design tokens. One cool neutral base, one ink for primary actions and a single lamp amber that
 * only ever means "live": connected, streaming, controlling or transferring.
 */
final class Theme {
    static boolean dark;
    static float density = 1;
    /** Page background, raised surfaces, inset fill (inputs, idle tiles), hairline and specular rim. */
    static int BG, SURFACE, SURFACE_RAISED, INSET, LINE, RIM;
    /** Text: primary ink, secondary muted, tertiary faint; ON_INK is text placed on an INK fill. */
    static int INK, MUTED, FAINT, ON_INK;
    /** The lamp. LAMP_SOFT is its tinted wash, LAMP_GLOW is its active rim, LAMP_TEXT is readable amber text. */
    static int LAMP, LAMP_SOFT, LAMP_GLOW, LAMP_TEXT, ON_LAMP;
    /** Navigation active capsule fill and warm tile surface. */
    static int NAV_PILL, TILE_ON, TILE_ON_GRAD;
    static int POSITIVE, POSITIVE_SOFT, DANGER, DANGER_SOFT, WARN, WARN_SOFT;
    /** Scrim behind sheets; dark frosted chrome used over live video. */
    static int SCRIM, VIDEO_CHROME, VIDEO_CHROME_STROKE, VIDEO_CHROME_ACTIVE, VIDEO_CHROME_LINE;
    static final Typeface REGULAR = Typeface.create("sans-serif", Typeface.NORMAL);
    static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);
    static final Typeface MONO = Typeface.MONOSPACE;

    private Theme() {}

    static void init(Context context) {
        density = context.getResources().getDisplayMetrics().density;
        dark = (context.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) ==
               Configuration.UI_MODE_NIGHT_YES;
        if (dark) {
            BG = 0xff0b0f14;
            SURFACE = 0xff131a23;
            SURFACE_RAISED = 0xff1a2330;
            INSET = 0xff1b2431;
            LINE = 0xff253140;
            RIM = 0x1cffffff;
            INK = 0xfff0f4f8;
            MUTED = 0xff94a3b8;
            FAINT = 0xff627184;
            ON_INK = 0xff0e141c;
            LAMP = 0xfff7b23b;
            LAMP_SOFT = 0x2bf7b23b;
            LAMP_GLOW = 0x66f7b23b;
            LAMP_TEXT = 0xfffac666;
            ON_LAMP = 0xff12171e;
            NAV_PILL = 0x26f7b23b;
            TILE_ON = 0xff22201b;
            TILE_ON_GRAD = 0xff191d25;
            POSITIVE = 0xff46c89a;
            POSITIVE_SOFT = 0x2646c89a;
            DANGER = 0xffff6b6b;
            DANGER_SOFT = 0x26ff6b6b;
            WARN = 0xfff2a649;
            WARN_SOFT = 0x26f2a649;
            SCRIM = 0xad04070b;
        } else {
            BG = 0xfff1f4f8;
            SURFACE = 0xffffffff;
            SURFACE_RAISED = 0xfff8fafc;
            INSET = 0xffe8eef5;
            LINE = 0xffdbe2eb;
            RIM = 0xffe0e6ef;
            INK = 0xff151e29;
            MUTED = 0xff5a6778;
            FAINT = 0xff8d99a8;
            ON_INK = 0xffffffff;
            LAMP = 0xffe89210;
            LAMP_SOFT = 0x22e89210;
            LAMP_GLOW = 0x66e89210;
            LAMP_TEXT = 0xff9a5700;
            ON_LAMP = 0xffffffff;
            NAV_PILL = 0x22e89210;
            TILE_ON = 0xfffff8eb;
            TILE_ON_GRAD = 0xffffffff;
            POSITIVE = 0xff1c825c;
            POSITIVE_SOFT = 0x1c1c825c;
            DANGER = 0xffcf3832;
            DANGER_SOFT = 0x1acf3832;
            WARN = 0xffb5610a;
            WARN_SOFT = 0x1eb5610a;
            SCRIM = 0x660b121b;
        }
        VIDEO_CHROME = 0xdc0f151e;
        VIDEO_CHROME_STROKE = 0x30ffffff;
        VIDEO_CHROME_ACTIVE = 0x33f7b23b;
        VIDEO_CHROME_LINE = 0x29ffffff;
    }

    static int dp(float value) { return Math.round(value * density); }

    static int alpha(int color, float alpha) {
        int a = Math.max(0, Math.min(255, Math.round(alpha * 255)));
        return (a << 24) | (color & 0x00ffffff);
    }

    static int blend(int from, int to, float ratio) {
        float t = Math.max(0f, Math.min(1f, ratio));
        int a = Math.round(((from >>> 24) & 0xff) + (((to >>> 24) & 0xff) - ((from >>> 24) & 0xff)) * t);
        int r = Math.round(((from >>> 16) & 0xff) + (((to >>> 16) & 0xff) - ((from >>> 16) & 0xff)) * t);
        int g = Math.round(((from >>> 8) & 0xff) + (((to >>> 8) & 0xff) - ((from >>> 8) & 0xff)) * t);
        int b = Math.round((from & 0xff) + ((to & 0xff) - (from & 0xff)) * t);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
}
