package org.lantern.client;

import android.content.Context;
import android.graphics.*;
import android.view.View;

/** The connection lamp: the one expressive element. Dark when idle, pulsing while connecting, lit when live. */
final class Lamp extends View {
    static final int OFF = 0, BUSY = 1, ON = 2, ERROR = 3;
    int state = OFF;
    final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    long started = System.nanoTime();

    Lamp(Context c) { super(c); setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO); }

    void set(int value) {
        if (state == value) return;
        state = value;
        started = System.nanoTime();
        invalidate();
    }

    @Override protected void onDraw(Canvas canvas) {
        float cx = getWidth() / 2f, cy = getHeight() / 2f, max = Math.min(cx, cy);
        float core = max * 0.34f;
        paint.setShader(null);
        paint.setStyle(Paint.Style.FILL);
        if (state == ON || state == BUSY) {
            float glow = 1f;
            if (state == BUSY) {
                double t = (System.nanoTime() - started) / 1e9;
                glow = 0.32f + 0.68f * (float)(0.5 + 0.5 * Math.sin(t * Math.PI * 1.7));
                postInvalidateOnAnimation();
            }
            // Layered warm lantern halo
            int outer = Theme.alpha(Theme.LAMP, (Theme.dark ? 0.46f : 0.36f) * glow);
            int mid = Theme.alpha(Theme.LAMP, (Theme.dark ? 0.16f : 0.12f) * glow);
            paint.setShader(new RadialGradient(cx, cy, max, new int[] {outer, mid, 0},
                                               new float[] {0.22f, 0.58f, 1f}, Shader.TileMode.CLAMP));
            canvas.drawCircle(cx, cy, max, paint);
            paint.setShader(null);
            // Soft warm glass ring around the core
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Theme.dp(1.25f));
            paint.setColor(Theme.alpha(Theme.LAMP, (state == ON ? 0.45f : 0.25f + 0.3f * glow)));
            canvas.drawCircle(cx, cy, core * 1.48f, paint);
            // Luminous amber core with soft warm specular highlight
            paint.setStyle(Paint.Style.FILL);
            int centerColor = state == ON ? 0xffffdfa0 : Theme.alpha(0xffffdfa0, 0.6f + 0.4f * glow);
            int edgeColor = state == ON ? Theme.LAMP : Theme.alpha(Theme.LAMP, 0.55f + 0.45f * glow);
            paint.setShader(new RadialGradient(cx - core * 0.24f, cy - core * 0.28f, core * 1.25f,
                                               new int[] {centerColor, edgeColor}, null, Shader.TileMode.CLAMP));
            canvas.drawCircle(cx, cy, core, paint);
            paint.setShader(null);
            paint.setColor(0x7affffff);
            canvas.drawCircle(cx - core * 0.28f, cy - core * 0.30f, core * 0.28f, paint);
        } else {
            paint.setColor(state == ERROR ? Theme.DANGER_SOFT : Theme.INSET);
            canvas.drawCircle(cx, cy, core * 1.95f, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Theme.dp(1f));
            paint.setColor(state == ERROR ? Theme.alpha(Theme.DANGER, 0.45f) : Theme.RIM);
            canvas.drawCircle(cx, cy, core * 1.95f, paint);
            paint.setStrokeWidth(Theme.dp(1.6f));
            paint.setColor(state == ERROR ? Theme.DANGER : Theme.FAINT);
            canvas.drawCircle(cx, cy, core, paint);
        }
    }
}
