package org.lantern.client;

import android.content.Context;
import android.graphics.*;
import android.media.MediaCodec;
import android.view.*;

/**
 * Desktop picture on a SurfaceView: the decoder renders straight to a composer layer, so the phone
 * can present every decoded frame (up to its refresh rate) without a GPU copy through the view tree.
 * Zoom and pan use the view's scale/translation, which SurfaceView applies in sync with drawing.
 * The surface sits below the window, so the page around the preview card hides any zoomed overflow.
 */
final class AvcView extends SurfaceView implements SurfaceHolder.Callback {
    /** One lifetime of the SurfaceView's surface. The decoder holds a lease while it renders to it. */
    static final class Target {
        final Surface surface;
        int references = 1;
        volatile boolean visible = true;
        MediaCodec codec;       // decoder currently rendering here, guarded by this
        Surface placeholder;    // parked output while the real surface is gone
        SurfaceTexture parked;
        Target(Surface s) { surface = s; }
        synchronized void retain() {
            if (references == 0)
                throw new IllegalStateException("Released surface");
            references++;
        }
        synchronized void release() {
            if (--references == 0 && placeholder != null) {
                placeholder.release(); parked.release(); placeholder = null; parked = null;
            }
        }
        synchronized void attach(MediaCodec c) { codec = c; }
        synchronized void detach(MediaCodec c) { if (codec == c) codec = null; }
        /** Called before the surface is destroyed: the decoder must not touch it afterwards. */
        synchronized void park() {
            visible = false;
            if (codec == null) return;
            try {
                if (placeholder == null) { parked = new SurfaceTexture(false); placeholder = new Surface(parked); }
                codec.setOutputSurface(placeholder);
            } catch (RuntimeException released) {
                codec = null; // the decoder is already shutting down
            }
        }
    }
    Target current;
    final long[] updateTimes = new long[768];
    long updates;
    synchronized long updateCount() { return updates; }
    synchronized long[] updateStamps() {
        int n = (int)Math.min(updates, updateTimes.length);
        long[] result = new long[n];
        for (int i = 0; i < n; i++) result[i] = updateTimes[(int)((updates - n + i) % updateTimes.length)];
        return result;
    }
    /** A frame reached the display (decoder render callback). */
    synchronized void rendered(long nanos) { updateTimes[(int)(updates++ % updateTimes.length)] = nanos; }
    Runnable changed;
    DesktopViewport viewport;
    AvcView(Context context) {
        super(context);
        getHolder().addCallback(this);
        setPivotX(0);
        setPivotY(0);
    }
    synchronized Target acquire() {
        if (current == null || !current.visible)
            return null;
        current.retain();
        return current;
    }
    synchronized boolean isCurrent(Target value) {
        return current == value;
    }
    synchronized void closeSurface() {
        if (current != null) {
            current.park();
            current.release();
            current = null;
        }
    }
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        synchronized (this) {
            if (current != null) { current.park(); current.release(); }
            current = new Target(holder.getSurface());
        }
        updateViewport();
        if (changed != null)
            changed.run();
    }
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        updateViewport();
    }
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        synchronized (this) {
            if (current != null) current.park();
        }
        if (changed != null)
            changed.run();
    }
    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        updateViewport();
    }
    void viewport(DesktopViewport geometry) {
        viewport = geometry;
        updateViewport();
    }
    void updateViewport() {
        if (getWidth() == 0 || getHeight() == 0 || viewport == null || !viewport.valid)
            return;
        float sx = viewport.imageWidth * viewport.scale / getWidth(), sy = viewport.imageHeight * viewport.scale / getHeight();
        setScaleX(sx);
        setScaleY(sy);
        setTranslationX(viewport.left);
        setTranslationY(viewport.top);
        // Keep the composer layer inside the view's own box (its parent may be smaller when zoomed).
        View parent = (View) getParent();
        if (parent != null && sx > 0 && sy > 0) {
            int l = (int) Math.floor(-viewport.left / sx), t = (int) Math.floor(-viewport.top / sy);
            int r = (int) Math.ceil((parent.getWidth() - viewport.left) / sx), b = (int) Math.ceil((parent.getHeight() - viewport.top) / sy);
            setClipBounds(new Rect(Math.max(0, l), Math.max(0, t), Math.min(getWidth(), r), Math.min(getHeight(), b)));
        }
    }
}
