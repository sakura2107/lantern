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
    /** Page background, raised surface, inset fill (inputs, idle tiles) and hairline. */
    static int BG, SURFACE, INSET, LINE;
    /** Text: primary ink, secondary muted, tertiary faint; ON_INK is text placed on an INK fill. */
    static int INK, MUTED, FAINT, ON_INK;
    /** The lamp. LAMP_SOFT is its glow / tinted background, LAMP_TEXT is readable amber text. */
    static int LAMP, LAMP_SOFT, LAMP_TEXT;
    static int POSITIVE, POSITIVE_SOFT, DANGER, DANGER_SOFT, WARN, WARN_SOFT;
    /** Scrim behind sheets; chrome used over live video. */
    static int SCRIM, VIDEO_CHROME, VIDEO_CHROME_LINE;
    static final Typeface REGULAR = Typeface.create("sans-serif", Typeface.NORMAL);
    static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);
    static final Typeface MONO = Typeface.MONOSPACE;

    private Theme() {}

    static void init(Context context) {
        density = context.getResources().getDisplayMetrics().density;
        dark = (context.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) ==
               Configuration.UI_MODE_NIGHT_YES;
        if (dark) {
            BG = 0xff0e1319; SURFACE = 0xff161c24; INSET = 0xff1d2530; LINE = 0xff27313d;
            INK = 0xffe9edf2; MUTED = 0xff97a1ae; FAINT = 0xff66717e; ON_INK = 0xff10151b;
            LAMP = 0xfff6ad3c; LAMP_SOFT = 0x33f6ad3c; LAMP_TEXT = 0xfff7bb5c;
            POSITIVE = 0xff4cc196; POSITIVE_SOFT = 0x2b4cc196;
            DANGER = 0xffff6b61; DANGER_SOFT = 0x2bff6b61;
            WARN = 0xffeea14a; WARN_SOFT = 0x2beea14a;
            SCRIM = 0x99000000;
        } else {
            BG = 0xffeef1f4; SURFACE = 0xffffffff; INSET = 0xffe6eaef; LINE = 0xffdde2e8;
            INK = 0xff1b2430; MUTED = 0xff667180; FAINT = 0xff9aa3ae; ON_INK = 0xffffffff;
            LAMP = 0xfff3a223; LAMP_SOFT = 0x2ef3a223; LAMP_TEXT = 0xffa86400;
            POSITIVE = 0xff23845f; POSITIVE_SOFT = 0x1f23845f;
            DANGER = 0xffcf3d35; DANGER_SOFT = 0x1acf3d35;
            WARN = 0xffb8650f; WARN_SOFT = 0x1fb8650f;
            SCRIM = 0x66101820;
        }
        VIDEO_CHROME = 0xe0161c24;
        VIDEO_CHROME_LINE = 0x33ffffff;
    }

    static int dp(float value) { return Math.round(value * density); }

    static int alpha(int color, float alpha) {
        return (Math.round(alpha * 255) << 24) | (color & 0x00ffffff);
    }
}
