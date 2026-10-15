package org.lantern.client;

import android.content.Context;
import android.graphics.*;
import android.view.*;

/** Owns SurfaceTexture references; decoder and UI may release their leases independently. */
final class AvcView extends TextureView implements TextureView.SurfaceTextureListener {
    static final class Target {
        final SurfaceTexture texture;
        final Surface surface;
        int references = 1;
        volatile boolean visible = true;
        Target(SurfaceTexture t) {
            texture = t;
            surface = new Surface(t);
        }
        synchronized void retain() {
            if (references == 0)
                throw new IllegalStateException("Released surface");
            references++;
        }
        synchronized void release() {
            if (--references == 0) {
                surface.release();
                texture.release();
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
    Runnable changed;
    DesktopViewport viewport;
    AvcView(Context context) {
        super(context);
        setOpaque(false);
        setSurfaceTextureListener(this);
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
            current.release();
            current = null;
        }
    }
    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture texture, int width, int height) {
        synchronized (this) {
            if (current != null)
                current.release();
            current = new Target(texture);
        }
        updateViewport();
        if (changed != null)
            changed.run();
    }
    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture texture) {
        synchronized (this) {
            if (current != null && current.texture == texture)
                current.visible = false;
        }
        // Keep the decoder surface alive while hidden. Decode continues without rendering.
        if (changed != null)
            changed.run();
        return false;
    }
    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int width, int height) {
        updateViewport();
    }
    @Override
    public synchronized void onSurfaceTextureUpdated(SurfaceTexture texture) {
        updateTimes[(int)(updates++ % updateTimes.length)] = System.nanoTime();
    }
    void viewport(DesktopViewport geometry) {
        viewport = geometry;
        updateViewport();
    }
    void updateViewport() {
        if (getWidth() == 0 || getHeight() == 0 || viewport == null || !viewport.valid)
            return;
        Matrix matrix = new Matrix();
        matrix.setScale(viewport.imageWidth * viewport.scale / getWidth(), viewport.imageHeight * viewport.scale / getHeight());
        matrix.postTranslate(viewport.left, viewport.top);
        setTransform(matrix);
    }
}
