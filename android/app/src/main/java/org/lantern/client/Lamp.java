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
        float core = max * 0.36f;
        paint.setShader(null);
        paint.setStyle(Paint.Style.FILL);
        if (state == ON || state == BUSY) {
            float glow = 1f;
            if (state == BUSY) {
                double t = (System.nanoTime() - started) / 1e9;
                glow = 0.35f + 0.65f * (float)(0.5 + 0.5 * Math.sin(t * Math.PI * 1.6));
                postInvalidateOnAnimation();
            }
            int halo = Theme.alpha(Theme.LAMP, 0.42f * glow);
            paint.setShader(new RadialGradient(cx, cy, max, new int[] {halo, Theme.alpha(Theme.LAMP, 0.12f * glow), 0},
                                               new float[] {0.25f, 0.62f, 1f}, Shader.TileMode.CLAMP));
            canvas.drawCircle(cx, cy, max, paint);
            paint.setShader(null);
            paint.setColor(state == ON ? Theme.LAMP : Theme.alpha(Theme.LAMP, 0.55f + 0.45f * glow));
            canvas.drawCircle(cx, cy, core, paint);
            paint.setColor(0x66ffffff);
            canvas.drawCircle(cx - core * 0.28f, cy - core * 0.3f, core * 0.32f, paint);
        } else {
            paint.setColor(state == ERROR ? Theme.DANGER_SOFT : Theme.INSET);
            canvas.drawCircle(cx, cy, core * 1.9f, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Theme.dp(1.5f));
            paint.setColor(state == ERROR ? Theme.DANGER : Theme.FAINT);
            canvas.drawCircle(cx, cy, core, paint);
        }
    }
}
