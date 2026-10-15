package org.lantern.client;

/** Source-pixel geometry shared by JPEG, TextureView and input; independent of ImageView drawables. */
final class DesktopViewport {
    int imageWidth, imageHeight, viewWidth, viewHeight;
    float scale, left, top, panX, panY;
    boolean valid;
    void update(int iw, int ih, int vw, int vh, float zoom, float x, float y) {
        valid = iw > 1 && ih > 1 && vw > 0 && vh > 0 && Float.isFinite(zoom) && Float.isFinite(x) && Float.isFinite(y);
        if (!valid) return;
        imageWidth = iw; imageHeight = ih; viewWidth = vw; viewHeight = vh;
        scale = Math.min((float)vw / iw, (float)vh / ih) * Math.max(1, Math.min(4, zoom));
        float extraX = Math.max(0, (iw * scale - vw) / 2f), extraY = Math.max(0, (ih * scale - vh) / 2f);
        panX = Math.max(-extraX, Math.min(extraX, x));
        panY = Math.max(-extraY, Math.min(extraY, y));
        left = (vw - iw * scale) / 2f + panX;
        top = (vh - ih * scale) / 2f + panY;
    }
    int[] pointer(float x, float y) {
        if (!valid || !Float.isFinite(x) || !Float.isFinite(y) || x < 0 || y < 0 || x >= viewWidth || y >= viewHeight) return null;
        float sourceX = (x - left) / scale, sourceY = (y - top) / scale;
        if (sourceX < 0 || sourceY < 0 || sourceX >= imageWidth || sourceY >= imageHeight) return null;
        return new int[] {Math.min(65535, Math.round(sourceX * 65535 / (imageWidth - 1))),
                          Math.min(65535, Math.round(sourceY * 65535 / (imageHeight - 1)))};
    }
    int[] dragPointer(float x, float y) {
        if (!valid || !Float.isFinite(x) || !Float.isFinite(y)) return null;
        float sx = Math.max(0, Math.min(imageWidth - 1, (x - left) / scale));
        float sy = Math.max(0, Math.min(imageHeight - 1, (y - top) / scale));
        return new int[]{Math.round(sx * 65535 / (imageWidth - 1)), Math.round(sy * 65535 / (imageHeight - 1))};
    }
}
