package org.lantern.client;

import android.content.Context;
import android.graphics.*;
import android.view.*;
import android.widget.ImageView;
import java.util.Locale;

/** Counts distinct image draws, not display refreshes or optical end-to-end latency. Main thread only. */
final class DesktopView extends ImageView {
    final long[] draws = new long[512];
    int drawCount;
    long generation, drawnGeneration;
    int imageWidth, imageHeight;
    final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG), background = new Paint(Paint.ANTI_ALIAS_FLAG),
                border = new Paint(Paint.ANTI_ALIAS_FLAG), liveDot = new Paint(Paint.ANTI_ALIAS_FLAG);
    final float density;
    interface Tap {
        void at(float x, float y);
    }
    Tap tap;
    Tap doubleTap;
    /** Quick two-finger tap (no movement, no zoom change) = right click at the midpoint. */
    Tap twoFingerTap;
    boolean twoCandidate;
    long twoDownAt;
    float twoZoom, tx0, ty0, tx1, ty1;
    interface Drag {
        boolean begin(float x, float y);
        void move(float x, float y);
        void end(float x, float y, boolean cancel);
    }
    Drag drag;
    interface Wheel {
        void move(float x, float y);
        void scroll(int delta);
    }
    Wheel wheel;
    final WheelGesture wheelGesture;
    boolean controlEnabled, dragging;
    float lastX, lastY, multiX, multiY;
    Runnable viewportChanged;
    String codecLabel = "JPEG";
    final Matrix transform = new Matrix();
    final DesktopViewport viewport = new DesktopViewport();
    final ScaleGestureDetector scale;
    final GestureDetector gestures;
    float zoom = 1, panX, panY;
    boolean multiple, touchActive;

    /**
     * Pure-geometry two-finger vertical wheel gesture classifier with throttling and clamping.
     * Distinguishes parallel vertical two-finger scroll from pinch-zoom and two-finger right-click.
     */
    static final class WheelGesture {
        static final int UNDECIDED = 0, WHEEL = 1, PINCH = 2;
        static final float MAX_WHEEL_ZOOM = 1.02f;
        static final float STEP_DP = 18f;
        static final int WHEEL_DELTA = 120;
        static final int MAX_DELTA = 1200;
        static final long MIN_EMIT_INTERVAL_MS = 32L;

        final float density;
        int state = UNDECIDED;
        float startX0, startY0, startX1, startY1, startSpan, startMidX, startMidY, lastMidY;
        float accumulatedPx;
        int pendingDelta;
        long lastEmitMs;
        boolean enteredWheel;

        WheelGesture(float density) {
            this.density = density > 0 ? density : 1f;
        }

        static int clampDelta(int delta) {
            return Math.max(-MAX_DELTA, Math.min(MAX_DELTA, delta));
        }

        void reset() {
            state = UNDECIDED;
            accumulatedPx = 0f;
            pendingDelta = 0;
            lastEmitMs = 0L;
            enteredWheel = false;
        }

        boolean isWheel() { return state == WHEEL; }
        boolean isPinch() { return state == PINCH; }

        boolean consumeEnteredWheel() {
            if (!enteredWheel) return false;
            enteredWheel = false;
            return true;
        }

        void begin(float x0, float y0, float x1, float y1, long nowMs, boolean enabled, float zoom) {
            reset();
            startX0 = x0; startY0 = y0; startX1 = x1; startY1 = y1;
            startSpan = (float)Math.hypot(x1 - x0, y1 - y0);
            startMidX = (x0 + x1) * 0.5f;
            startMidY = (y0 + y1) * 0.5f;
            lastMidY = startMidY;
            if (!enabled || zoom > MAX_WHEEL_ZOOM) state = PINCH;
        }

        /** Updates with two-finger coordinates; returns a clamped WHEEL_DELTA to send now (or 0). */
        int move(float x0, float y0, float x1, float y1, long nowMs, boolean enabled, float zoom) {
            if (!enabled || zoom > MAX_WHEEL_ZOOM) {
                state = PINCH;
                pendingDelta = 0;
                return 0;
            }
            if (state == PINCH) return 0;
            float curSpan = (float)Math.hypot(x1 - x0, y1 - y0);
            float spanDelta = Math.abs(curSpan - startSpan);
            float spanRatio = spanDelta / Math.max(startSpan, 40f * density);
            float dy0 = y0 - startY0, dy1 = y1 - startY1;
            float dx0 = x0 - startX0, dx1 = x1 - startX1;
            float avgDy = (dy0 + dy1) * 0.5f;
            float avgDx = (dx0 + dx1) * 0.5f;
            float midY = (y0 + y1) * 0.5f;
            if (state == UNDECIDED) {
                if ((spanDelta > 18f * density && spanRatio > 0.14f) ||
                    (spanDelta > 14f * density && spanDelta > Math.abs(avgDy) * 0.85f)) {
                    state = PINCH;
                    return 0;
                }
                boolean sameY = (dy0 > 0f && dy1 > 0f) || (dy0 < 0f && dy1 < 0f);
                boolean enoughY = Math.abs(avgDy) >= 12f * density &&
                                  Math.min(Math.abs(dy0), Math.abs(dy1)) >= 6f * density;
                boolean vertical = Math.abs(avgDy) >= Math.abs(avgDx) * 1.25f;
                boolean steadySpan = spanRatio < 0.16f && spanDelta < Math.abs(avgDy) * 0.65f;
                if (!sameY || !enoughY || !vertical || !steadySpan) return 0;
                state = WHEEL;
                enteredWheel = true;
                accumulatedPx = avgDy;
                lastMidY = midY;
            } else {
                accumulatedPx += (midY - lastMidY);
                lastMidY = midY;
            }
            float stepPx = STEP_DP * density;
            int steps = (int)(accumulatedPx / stepPx);
            if (steps != 0) {
                accumulatedPx -= steps * stepPx;
                pendingDelta = clampDelta(pendingDelta + steps * WHEEL_DELTA);
            }
            if (pendingDelta != 0 &&
                (lastEmitMs == 0L || nowMs - lastEmitMs >= MIN_EMIT_INTERVAL_MS || Math.abs(pendingDelta) >= 360)) {
                int emit = clampDelta(pendingDelta);
                pendingDelta = 0;
                lastEmitMs = nowMs;
                return emit;
            }
            return 0;
        }

        /** Flushes any buffered notch on normal release; cancels cleanly on ACTION_CANCEL. */
        int finish(boolean cancel) {
            int emit = (!cancel && state == WHEEL && pendingDelta != 0) ? clampDelta(pendingDelta) : 0;
            reset();
            return emit;
        }
    }

    DesktopView(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        wheelGesture = new WheelGesture(density);
        label.setColor(0xf2ffffff);
        label.setTypeface(Theme.MEDIUM);
        label.setFontFeatureSettings("tnum");
        label.setTextSize(11.5f * getResources().getDisplayMetrics().scaledDensity);
        background.setColor(0xcc0f151e);
        border.setStyle(Paint.Style.STROKE);
        border.setStrokeWidth(Math.max(1f, density));
        border.setColor(0x33ffffff);
        liveDot.setColor(Theme.LAMP);
        setScaleType(ScaleType.MATRIX);
        scale = new ScaleGestureDetector(context, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override
            public boolean onScale(ScaleGestureDetector detector) {
                if (wheelGesture.isWheel()) return false;
                float next = Math.max(1, Math.min(4, zoom * detector.getScaleFactor()));
                float ratio = next / zoom;
                panX = (panX - detector.getFocusX() + getWidth() / 2f) * ratio + detector.getFocusX() -
                       getWidth() / 2f;
                panY = (panY - detector.getFocusY() + getHeight() / 2f) * ratio + detector.getFocusY() -
                       getHeight() / 2f;
                zoom = next;
                updateTransform();
                return true;
            }
        });
        gestures = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDown(android.view.MotionEvent e) {
                return true;
            }
            @Override
            public boolean onSingleTapConfirmed(android.view.MotionEvent e) {
                if (!multiple && tap != null)
                    tap.at(e.getX(), e.getY());
                return true;
            }
            @Override
            public boolean onDoubleTap(android.view.MotionEvent e) {
                if (!multiple && doubleTap != null) doubleTap.at(e.getX(), e.getY());
                return true;
            }
            @Override
            public void onLongPress(android.view.MotionEvent e) {
                if (!multiple && controlEnabled && drag != null)
                    dragging = drag.begin(e.getX(), e.getY());
            }
            @Override
            public boolean onScroll(android.view.MotionEvent first, android.view.MotionEvent current,
                                    float dx, float dy) {
                if (!scale.isInProgress() && !multiple && controlEnabled && drag != null) {
                    if (!dragging) dragging = drag.begin(first.getX(), first.getY());
                    if (dragging) drag.move(current.getX(), current.getY());
                } else if (!scale.isInProgress() && zoom > 1 && !multiple) {
                    panX -= dx;
                    panY -= dy;
                    updateTransform();
                }
                return true;
            }
        });
    }
    @Override
    public void setImageBitmap(Bitmap bitmap) {
        bitmap.setDensity(Bitmap.DENSITY_NONE); // Source pixels must not become density-scaled drawable units.
        codecLabel = "JPEG";
        generation++;
        imageWidth = bitmap.getWidth();
        imageHeight = bitmap.getHeight();
        super.setImageBitmap(bitmap);
        updateTransform();
    }
    void videoSize(int width, int height) {
        codecLabel = "H264";
        setImageDrawable(null);
        imageWidth = width;
        imageHeight = height;
        updateTransform();
    }
    void videoFrame() {
        generation++;
        invalidate();
    }
    void updateTransform() {
        if (!Float.isFinite(zoom) || !Float.isFinite(panX) || !Float.isFinite(panY)) {
            zoom = 1;
            panX = panY = 0;
        }
        viewport.update(imageWidth, imageHeight, getWidth(), getHeight(), zoom, panX, panY);
        if (!viewport.valid)
            return;
        panX = viewport.panX; panY = viewport.panY;
        transform.setScale(viewport.scale, viewport.scale);
        transform.postTranslate(viewport.left, viewport.top);
        setImageMatrix(transform);
        if (viewportChanged != null)
            viewportChanged.run();
    }
    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        if (w != oldw || h != oldh) resetGesture();
        super.onSizeChanged(w, h, oldw, oldh);
        updateTransform();
    }
    @Override
    public boolean onTouchEvent(android.view.MotionEvent event) {
        int action = event.getActionMasked();
        if (action == android.view.MotionEvent.ACTION_DOWN) {
            if (touchActive) resetGesture();
            touchActive = true;
            multiple = false;
            twoCandidate = false;
            wheelGesture.reset();
        }
        if (!touchActive) return true;
        if (action == MotionEvent.ACTION_POINTER_DOWN && event.getPointerCount() == 2) {
            twoCandidate = true;
            twoDownAt = event.getEventTime();
            twoZoom = zoom;
            tx0 = event.getX(0); ty0 = event.getY(0); tx1 = event.getX(1); ty1 = event.getY(1);
            wheelGesture.begin(tx0, ty0, tx1, ty1, twoDownAt, controlEnabled, zoom);
        } else if (event.getPointerCount() > 2) {
            twoCandidate = false;
            wheelGesture.reset();
        } else if (twoCandidate && action == MotionEvent.ACTION_MOVE && event.getPointerCount() == 2) {
            float slop = 12 * density;
            if (Math.hypot(event.getX(0) - tx0, event.getY(0) - ty0) > slop ||
                Math.hypot(event.getX(1) - tx1, event.getY(1) - ty1) > slop)
                twoCandidate = false;
        } else if (twoCandidate && action == MotionEvent.ACTION_POINTER_UP) {
            twoCandidate = false;
            if (event.getEventTime() - twoDownAt < 300 && zoom == twoZoom && !wheelGesture.isWheel() &&
                controlEnabled && twoFingerTap != null)
                twoFingerTap.at((tx0 + tx1) / 2, (ty0 + ty1) / 2);
        }
        lastX = event.getX(); lastY = event.getY();
        if (event.getPointerCount() > 1) {
            multiple = true;
            cancelDrag();
            float x = (event.getX(0) + event.getX(1)) / 2, y = (event.getY(0) + event.getY(1)) / 2;
            if (action == MotionEvent.ACTION_MOVE && event.getPointerCount() == 2) {
                if (controlEnabled && zoom <= WheelGesture.MAX_WHEEL_ZOOM) {
                    int delta = wheelGesture.move(event.getX(0), event.getY(0), event.getX(1), event.getY(1),
                                                  event.getEventTime(), controlEnabled, zoom);
                    if (wheelGesture.isWheel()) {
                        twoCandidate = false;
                        if (wheelGesture.consumeEnteredWheel() && wheel != null)
                            wheel.move(wheelGesture.startMidX, wheelGesture.startMidY);
                        if (delta != 0 && wheel != null) wheel.scroll(delta);
                    }
                } else if (zoom > WheelGesture.MAX_WHEEL_ZOOM || (!controlEnabled && zoom > 1)) {
                    panX += x - multiX; panY += y - multiY; updateTransform();
                }
            }
            multiX = x; multiY = y;
        }
        if (action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            if (wheelGesture.isWheel()) {
                int tail = wheelGesture.finish(action == MotionEvent.ACTION_CANCEL);
                if (tail != 0 && wheel != null) wheel.scroll(tail);
            } else if (action != MotionEvent.ACTION_POINTER_UP || event.getPointerCount() <= 2) {
                wheelGesture.reset();
            }
        }
        if (!wheelGesture.isWheel()) scale.onTouchEvent(event);
        gestures.onTouchEvent(event);
        if (dragging && action == MotionEvent.ACTION_MOVE && !multiple) drag.move(lastX, lastY);
        if (dragging && (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL)) {
            drag.end(lastX, lastY, action == MotionEvent.ACTION_CANCEL); dragging = false;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            touchActive = false;
            twoCandidate = false;
            wheelGesture.reset();
        }
        return true;
    }
    void resetGesture() {
        long now = android.os.SystemClock.uptimeMillis();
        MotionEvent cancel = MotionEvent.obtain(now, now, MotionEvent.ACTION_CANCEL, 0f, 0f, 0);
        gestures.onTouchEvent(cancel);
        scale.onTouchEvent(cancel);
        cancel.recycle();
        cancelDrag();
        wheelGesture.reset();
        multiple = twoCandidate = touchActive = false;
    }
    @Override protected void onDetachedFromWindow() {
        resetGesture();
        super.onDetachedFromWindow();
    }
    void cancelDrag() {
        if (dragging && drag != null) drag.end(lastX, lastY, true);
        dragging = false;
    }
    void resetDrawMetrics() {
        drawCount = 0;
    }
    long[] drawTimes() {
        int n = Math.min(drawCount, draws.length);
        long[] result = new long[n];
        for (int i = 0; i < n; i++)
            result[i] = draws[(drawCount - n + i) % draws.length];
        return result;
    }
    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        long now = System.nanoTime();
        if (drawnGeneration != generation) {
            drawnGeneration = generation;
            draws[drawCount++ % draws.length] = now;
        }
        int n = Math.min(drawCount, draws.length), first = drawCount - n;
        while (first < drawCount - 1 && now - draws[first % draws.length] > 1_000_000_000L)
            first++;
        double fps = drawCount - first > 1
                         ? (drawCount - first - 1) * 1e9 / Math.max(1, now - draws[first % draws.length])
                         : 0;
        String text =
            String.format(Locale.ROOT, "%s %d×%d · %.1f fps", codecLabel, imageWidth, imageHeight, fps);
        // Frosted dark glass pill inset from the rounded preview corner with a live lantern dot.
        float x = 12 * density, y = 12 * density, padL = 9 * density, padR = 10 * density, h = 24 * density;
        float dotR = 2.8f * density, dotGap = 6 * density;
        float right = x + padL + dotR * 2 + dotGap + label.measureText(text) + padR;
        canvas.drawRoundRect(x, y, right, y + h, h / 2, h / 2, background);
        canvas.drawRoundRect(x, y, right, y + h, h / 2, h / 2, border);
        liveDot.setColor(fps >= 1 ? Theme.LAMP : 0x80ffffff);
        canvas.drawCircle(x + padL + dotR, y + h / 2, dotR, liveDot);
        canvas.drawText(text, x + padL + dotR * 2 + dotGap, y + h / 2 - (label.ascent() + label.descent()) / 2, label);
    }
}
