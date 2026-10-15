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
    final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG), background = new Paint();
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
    boolean controlEnabled, dragging;
    float lastX, lastY, multiX, multiY;
    Runnable viewportChanged;
    String codecLabel = "JPEG";
    final Matrix transform = new Matrix();
    final DesktopViewport viewport = new DesktopViewport();
    final ScaleGestureDetector scale;
    final GestureDetector gestures;
    float zoom = 1, panX, panY;
    boolean multiple;
    DesktopView(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        label.setColor(Color.WHITE);
        label.setTextSize(12 * getResources().getDisplayMetrics().scaledDensity);
        background.setColor(0xa0000000);
        background.setAntiAlias(true);
        setScaleType(ScaleType.MATRIX);
        scale = new ScaleGestureDetector(context, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override
            public boolean onScale(ScaleGestureDetector detector) {
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
        if (w != oldw || h != oldh) cancelDrag();
        super.onSizeChanged(w, h, oldw, oldh);
        updateTransform();
    }
    @Override
    public boolean onTouchEvent(android.view.MotionEvent event) {
        int action = event.getActionMasked();
        if (action == android.view.MotionEvent.ACTION_DOWN) {
            cancelDrag();
            multiple = false;
            twoCandidate = false;
        }
        if (action == MotionEvent.ACTION_POINTER_DOWN && event.getPointerCount() == 2) {
            twoCandidate = true;
            twoDownAt = event.getEventTime();
            twoZoom = zoom;
            tx0 = event.getX(0); ty0 = event.getY(0); tx1 = event.getX(1); ty1 = event.getY(1);
        } else if (event.getPointerCount() > 2) {
            twoCandidate = false;
        } else if (twoCandidate && action == MotionEvent.ACTION_MOVE && event.getPointerCount() == 2) {
            float slop = 12 * density;
            if (Math.hypot(event.getX(0) - tx0, event.getY(0) - ty0) > slop ||
                Math.hypot(event.getX(1) - tx1, event.getY(1) - ty1) > slop)
                twoCandidate = false;
        } else if (twoCandidate && action == MotionEvent.ACTION_POINTER_UP) {
            twoCandidate = false;
            if (event.getEventTime() - twoDownAt < 300 && zoom == twoZoom && controlEnabled && twoFingerTap != null)
                twoFingerTap.at((tx0 + tx1) / 2, (ty0 + ty1) / 2);
        }
        lastX = event.getX(); lastY = event.getY();
        if (event.getPointerCount() > 1) {
            multiple = true;
            cancelDrag();
            float x = (event.getX(0) + event.getX(1))/2, y = (event.getY(0) + event.getY(1))/2;
            if (action == MotionEvent.ACTION_MOVE && zoom > 1) {
                panX += x - multiX; panY += y - multiY; updateTransform();
            }
            multiX = x; multiY = y;
        }
        scale.onTouchEvent(event);
        gestures.onTouchEvent(event);
        if (dragging && action == MotionEvent.ACTION_MOVE && !multiple) drag.move(lastX, lastY);
        if (dragging && (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL)) {
            drag.end(lastX, lastY, action == MotionEvent.ACTION_CANCEL); dragging = false;
        }
        return true;
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
        // Small pill inset from the rounded preview corner.
        float x = 10 * density, y = 10 * density, pad = 8 * density, h = 22 * density;
        canvas.drawRoundRect(x, y, x + label.measureText(text) + pad * 2, y + h, h / 2, h / 2, background);
        canvas.drawText(text, x + pad, y + h / 2 - (label.ascent() + label.descent()) / 2, label);
    }
}
