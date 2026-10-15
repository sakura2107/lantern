package org.lantern.client;

import android.graphics.*;
import android.graphics.drawable.Drawable;

/** Line icons drawn on a 24-unit grid, so the app needs no bitmap or vector resources. */
final class Glyph extends Drawable {
    static final int PHONE = 0, DESKTOP = 1, FOLDER = 2, TRANSFER = 3, FILE = 4, VIDEO = 5, IMAGE = 6,
                     AUDIO = 7, DOC = 8, ARCHIVE = 9, CHEVRON_RIGHT = 10, BACK = 11, SEARCH = 12,
                     SORT = 13, MORE = 14, PLUS = 15, UPLOAD = 16, DOWNLOAD = 17, CHECK = 18, CLOSE = 19,
                     PAUSE = 20, PLAY = 21, REFRESH = 22, RADAR = 23, KEY = 24, SHIELD = 25,
                     KEYBOARD = 26, POINTER = 27, SPEAKER = 28, SPEAKER_OFF = 29, EYE = 30, EYE_OFF = 31,
                     MONITORS = 32, SLIDERS = 33, EXPAND = 34, SHRINK = 35, ROTATE = 36, CLIPBOARD = 37,
                     COMMAND = 38, TRASH = 39, EDIT = 40, COPY = 41, MOVE = 42, INFO = 43, OPEN = 44,
                     FOLDER_PLUS = 45, SELECT = 46, PULSE = 47, BELL = 48, LICENSE = 49, TOOLS = 50,
                     CHEVRON_DOWN = 51, TOP = 52, STOP = 53, HOME = 54, PASTE = 55, CLOCK = 56,
                     WARNING = 57, SELECT_ALL = 58, LOCK = 59, CHEVRON_LEFT = 60, MOUSE_RIGHT = 61;

    final int kind;
    final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG), fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    final Path path = new Path();
    int size;

    Glyph(int kind, int color) { this(kind, color, Theme.dp(22)); }
    Glyph(int kind, int color, int size) {
        this.kind = kind;
        this.size = size;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(1.75f);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        fill.setStyle(Paint.Style.FILL);
        setColor(color);
    }
    Glyph setColor(int color) {
        stroke.setColor(color);
        fill.setColor(color);
        invalidateSelf();
        return this;
    }

    @Override public int getIntrinsicWidth() { return size; }
    @Override public int getIntrinsicHeight() { return size; }
    @Override public void setAlpha(int alpha) { stroke.setAlpha(alpha); fill.setAlpha(alpha); }
    @Override public void setColorFilter(ColorFilter filter) { stroke.setColorFilter(filter); fill.setColorFilter(filter); }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }

    void line(Canvas c, float x1, float y1, float x2, float y2) { c.drawLine(x1, y1, x2, y2, stroke); }
    void poly(Canvas c, boolean close, float... xy) {
        path.reset();
        path.moveTo(xy[0], xy[1]);
        for (int i = 2; i < xy.length; i += 2) path.lineTo(xy[i], xy[i + 1]);
        if (close) path.close();
        c.drawPath(path, stroke);
    }
    void rect(Canvas c, float l, float t, float r, float b, float radius) { c.drawRoundRect(l, t, r, b, radius, radius, stroke); }
    void dot(Canvas c, float x, float y, float r) { c.drawCircle(x, y, r, fill); }
    void arc(Canvas c, float cx, float cy, float r, float start, float sweep) {
        c.drawArc(cx - r, cy - r, cx + r, cy + r, start, sweep, false, stroke);
    }
    void folder(Canvas c) { poly(c, true, 3, 6.5f, 3, 5, 9, 5, 11, 7.5f, 21, 7.5f, 21, 19, 3, 19); }
    void page(Canvas c) {
        poly(c, true, 6, 3, 14, 3, 19, 8, 19, 21, 6, 21);
        poly(c, false, 14, 3, 14, 8, 19, 8);
    }

    @Override
    public void draw(Canvas c) {
        Rect b = getBounds();
        c.save();
        c.translate(b.left, b.top);
        c.scale(b.width() / 24f, b.height() / 24f);
        switch (kind) {
        case PHONE: rect(c, 6.5f, 2.5f, 17.5f, 21.5f, 2.5f); line(c, 10.5f, 18, 13.5f, 18); break;
        case DESKTOP: rect(c, 2.5f, 4, 21.5f, 16, 2); line(c, 12, 16, 12, 20); line(c, 8, 20, 16, 20); break;
        case FOLDER: folder(c); break;
        case TRANSFER: poly(c, false, 7, 20, 7, 4); poly(c, false, 3.5f, 7.5f, 7, 4, 10.5f, 7.5f);
            poly(c, false, 17, 4, 17, 20); poly(c, false, 13.5f, 16.5f, 17, 20, 20.5f, 16.5f); break;
        case FILE: page(c); break;
        case VIDEO: rect(c, 3, 5, 21, 19, 2.5f); poly(c, true, 10, 9, 15, 12, 10, 15); break;
        case IMAGE: rect(c, 3, 4, 21, 20, 2.5f); c.drawCircle(9, 9.5f, 1.6f, stroke);
            poly(c, false, 3.5f, 17, 9, 12.5f, 13, 16, 16, 13.5f, 20.5f, 17); break;
        case AUDIO: poly(c, false, 9, 17, 9, 5, 19, 3, 19, 15); c.drawCircle(7, 17, 2.2f, stroke);
            c.drawCircle(17, 15, 2.2f, stroke); break;
        case DOC: page(c); line(c, 9, 12.5f, 16, 12.5f); line(c, 9, 16, 16, 16); break;
        case ARCHIVE: rect(c, 4, 4, 20, 20, 2.5f); line(c, 12, 4, 12, 7); line(c, 12, 9, 12, 11);
            rect(c, 10.5f, 12.5f, 13.5f, 16, 1); break;
        case CHEVRON_RIGHT: poly(c, false, 9.5f, 6, 15.5f, 12, 9.5f, 18); break;
        case CHEVRON_LEFT: poly(c, false, 14.5f, 6, 8.5f, 12, 14.5f, 18); break;
        case CHEVRON_DOWN: poly(c, false, 6, 9.5f, 12, 15.5f, 18, 9.5f); break;
        case BACK: poly(c, false, 11, 5, 4, 12, 11, 19); line(c, 4.5f, 12, 20, 12); break;
        case SEARCH: c.drawCircle(10.5f, 10.5f, 6, stroke); line(c, 15, 15, 20, 20); break;
        case SORT: line(c, 4, 7, 14, 7); line(c, 4, 12, 11, 12); line(c, 4, 17, 8, 17);
            line(c, 18, 5, 18, 19); poly(c, false, 15, 16, 18, 19, 21, 16); break;
        case MORE: dot(c, 5.5f, 12, 1.6f); dot(c, 12, 12, 1.6f); dot(c, 18.5f, 12, 1.6f); break;
        case PLUS: line(c, 12, 5, 12, 19); line(c, 5, 12, 19, 12); break;
        case UPLOAD: line(c, 12, 15, 12, 4); poly(c, false, 7.5f, 8.5f, 12, 4, 16.5f, 8.5f);
            poly(c, false, 4, 15, 4, 20, 20, 20, 20, 15); break;
        case DOWNLOAD: line(c, 12, 4, 12, 15); poly(c, false, 7.5f, 10.5f, 12, 15, 16.5f, 10.5f);
            poly(c, false, 4, 15, 4, 20, 20, 20, 20, 15); break;
        case CHECK: poly(c, false, 5, 12.5f, 10, 17.5f, 19, 7); break;
        case CLOSE: line(c, 6, 6, 18, 18); line(c, 18, 6, 6, 18); break;
        case PAUSE: line(c, 9, 6, 9, 18); line(c, 15, 6, 15, 18); break;
        case PLAY: poly(c, true, 8, 5.5f, 18.5f, 12, 8, 18.5f); break;
        case STOP: rect(c, 6.5f, 6.5f, 17.5f, 17.5f, 2); break;
        case REFRESH: arc(c, 12, 12, 7.5f, -60, 300); poly(c, false, 16, 3.5f, 16, 7.5f, 20, 7.5f); break;
        case RADAR: dot(c, 12, 18, 1.6f); arc(c, 12, 18, 5, 225, 90); arc(c, 12, 18, 10, 225, 90); break;
        case KEY: c.drawCircle(8, 15, 4, stroke); line(c, 11, 12, 20, 3); line(c, 17, 6, 19.5f, 8.5f);
            line(c, 14.5f, 8.5f, 16.5f, 10.5f); break;
        case SHIELD: poly(c, true, 12, 3, 19.5f, 6, 19.5f, 11.5f, 12, 21, 4.5f, 11.5f, 4.5f, 6);
            poly(c, false, 8.5f, 12, 11, 14.5f, 15.5f, 9.5f); break;
        case LOCK: rect(c, 5, 10.5f, 19, 20.5f, 2.5f); arc(c, 12, 10.5f, 4, 180, 180); line(c, 8, 10.5f, 8, 10);
            line(c, 16, 10.5f, 16, 10); break;
        case KEYBOARD: rect(c, 2.5f, 6, 21.5f, 18, 2.5f); dot(c, 6.5f, 10, 1); dot(c, 10, 10, 1); dot(c, 13.5f, 10, 1);
            dot(c, 17, 10, 1); line(c, 8, 14.5f, 16, 14.5f); break;
        case POINTER: poly(c, true, 5, 3.5f, 18.5f, 12, 12.5f, 13.5f, 10, 19.5f); break;
        case SPEAKER: poly(c, true, 4, 9.5f, 8, 9.5f, 13, 5, 13, 19, 8, 14.5f, 4, 14.5f);
            arc(c, 13, 12, 4, -50, 100); arc(c, 13, 12, 7.5f, -50, 100); break;
        case SPEAKER_OFF: poly(c, true, 4, 9.5f, 8, 9.5f, 13, 5, 13, 19, 8, 14.5f, 4, 14.5f);
            line(c, 16, 9.5f, 21, 14.5f); line(c, 21, 9.5f, 16, 14.5f); break;
        case EYE: path.reset(); path.moveTo(2.5f, 12); path.quadTo(12, 2.5f, 21.5f, 12); path.quadTo(12, 21.5f, 2.5f, 12);
            c.drawPath(path, stroke); c.drawCircle(12, 12, 3, stroke); break;
        case EYE_OFF: path.reset(); path.moveTo(2.5f, 12); path.quadTo(12, 2.5f, 21.5f, 12); path.quadTo(12, 21.5f, 2.5f, 12);
            c.drawPath(path, stroke); line(c, 4, 4, 20, 20); break;
        case MONITORS: rect(c, 2.5f, 5, 13.5f, 13, 1.5f); rect(c, 10.5f, 9, 21.5f, 17, 1.5f); line(c, 16, 17, 16, 20);
            line(c, 13, 20, 19, 20); break;
        case SLIDERS: line(c, 4, 7, 20, 7); line(c, 4, 17, 20, 17); c.drawCircle(9, 7, 2.3f, fill);
            c.drawCircle(15, 17, 2.3f, fill); break;
        case EXPAND: poly(c, false, 4, 9, 4, 4, 9, 4); poly(c, false, 15, 4, 20, 4, 20, 9);
            poly(c, false, 20, 15, 20, 20, 15, 20); poly(c, false, 9, 20, 4, 20, 4, 15); break;
        case SHRINK: poly(c, false, 9, 4, 9, 9, 4, 9); poly(c, false, 15, 4, 15, 9, 20, 9);
            poly(c, false, 20, 15, 15, 15, 15, 20); poly(c, false, 4, 15, 9, 15, 9, 20); break;
        case ROTATE: rect(c, 8, 3, 16, 15, 1.5f); arc(c, 12, 12, 8.5f, 20, 140); poly(c, false, 2.5f, 13, 3.6f, 16.5f, 7, 15.2f); break;
        case CLIPBOARD: rect(c, 5, 4.5f, 19, 21, 2.5f); rect(c, 9, 3, 15, 6.5f, 1.2f); line(c, 9, 12, 15, 12);
            line(c, 9, 16, 13, 16); break;
        case PASTE: rect(c, 5, 4.5f, 19, 21, 2.5f); rect(c, 9, 3, 15, 6.5f, 1.2f); line(c, 12, 10, 12, 17);
            poly(c, false, 9, 14, 12, 17, 15, 14); break;
        case COMMAND: rect(c, 3, 3, 21, 21, 4); line(c, 7.5f, 9, 10.5f, 12); line(c, 10.5f, 12, 7.5f, 15); line(c, 12.5f, 15.5f, 16.5f, 15.5f); break;
        case TRASH: line(c, 4, 6.5f, 20, 6.5f); poly(c, false, 9, 6.5f, 9, 4, 15, 4, 15, 6.5f);
            poly(c, false, 6, 6.5f, 7, 20.5f, 17, 20.5f, 18, 6.5f); line(c, 10, 10.5f, 10, 16.5f); line(c, 14, 10.5f, 14, 16.5f); break;
        case EDIT: poly(c, true, 4, 20, 4.5f, 16, 16, 4.5f, 19.5f, 8, 8, 19.5f); line(c, 13.5f, 7, 17, 10.5f); break;
        case COPY: rect(c, 8.5f, 8.5f, 20.5f, 20.5f, 2.5f); poly(c, false, 15.5f, 8.5f, 15.5f, 4.5f, 3.5f, 4.5f, 3.5f, 15.5f, 8.5f, 15.5f); break;
        case MOVE: folder(c); line(c, 9, 13.5f, 15, 13.5f); poly(c, false, 12.5f, 11, 15, 13.5f, 12.5f, 16); break;
        case FOLDER_PLUS: folder(c); line(c, 12, 10.5f, 12, 16.5f); line(c, 9, 13.5f, 15, 13.5f); break;
        case INFO: c.drawCircle(12, 12, 9, stroke); line(c, 12, 11, 12, 16.5f); dot(c, 12, 7.8f, 1.1f); break;
        case WARNING: poly(c, true, 12, 3.5f, 21.5f, 20, 2.5f, 20); line(c, 12, 9.5f, 12, 14); dot(c, 12, 17, 1.1f); break;
        case OPEN: poly(c, false, 13, 4, 20, 4, 20, 11); line(c, 20, 4, 11, 13);
            poly(c, false, 17, 14, 17, 20, 4, 20, 4, 7, 10, 7); break;
        case SELECT: rect(c, 4, 4, 20, 20, 4); poly(c, false, 8, 12, 11, 15, 16, 9); break;
        case SELECT_ALL: rect(c, 3, 7, 17, 21, 3); poly(c, false, 7, 7, 7, 3, 21, 3, 21, 17, 17, 17); poly(c, false, 6.5f, 14, 9, 16.5f, 13.5f, 11.5f); break;
        case PULSE: poly(c, false, 2.5f, 12, 7, 12, 9.5f, 6, 14, 18, 16.5f, 12, 21.5f, 12); break;
        case BELL: path.reset(); path.moveTo(5, 17); path.lineTo(19, 17); path.lineTo(17.5f, 15); path.lineTo(17.5f, 10.5f);
            path.quadTo(17.5f, 4.5f, 12, 4.5f); path.quadTo(6.5f, 4.5f, 6.5f, 10.5f); path.lineTo(6.5f, 15); path.close();
            c.drawPath(path, stroke); line(c, 10, 20, 14, 20); break;
        case LICENSE: page(c); poly(c, false, 9.5f, 13, 11.5f, 15, 15.5f, 11); break;
        case TOOLS: line(c, 4, 6, 20, 6); line(c, 4, 12, 20, 12); line(c, 4, 18, 20, 18);
            c.drawCircle(8, 6, 2, fill); c.drawCircle(16, 12, 2, fill); c.drawCircle(10, 18, 2, fill); break;
        case TOP: line(c, 5, 4, 19, 4); line(c, 12, 20, 12, 8.5f); poly(c, false, 7.5f, 13, 12, 8.5f, 16.5f, 13); break;
        case HOME: poly(c, false, 3.5f, 11, 12, 4, 20.5f, 11); poly(c, false, 6, 9.5f, 6, 20, 18, 20, 18, 9.5f); break;
        case MOUSE_RIGHT: rect(c, 6, 3, 18, 21, 6); line(c, 12, 3, 12, 10); line(c, 6, 10, 18, 10);
            path.reset(); path.moveTo(12.9f, 3.9f); path.lineTo(12.9f, 9.1f); path.lineTo(17.1f, 9.1f);
            path.lineTo(17.1f, 8.5f); path.quadTo(17.1f, 3.9f, 12.9f, 3.9f); path.close(); c.drawPath(path, fill); break;
        case CLOCK: c.drawCircle(12, 12, 9, stroke); poly(c, false, 12, 7, 12, 12, 15.5f, 14); break;
        default: rect(c, 4, 4, 20, 20, 3);
        }
        c.restore();
    }

    /** Picks a file glyph from the extension; purely presentational. */
    static int forFile(String name, boolean directory) {
        if (directory) return FOLDER;
        String ext = Ui.extension(name);
        switch (ext) {
        case "mp4": case "mkv": case "webm": case "avi": case "mov": case "m4v": case "ts": case "wmv": case "flv": return VIDEO;
        case "jpg": case "jpeg": case "png": case "gif": case "webp": case "bmp": case "heic": case "avif": case "tif": case "tiff": return IMAGE;
        case "mp3": case "flac": case "wav": case "aac": case "m4a": case "ogg": case "opus": case "ape": return AUDIO;
        case "zip": case "rar": case "7z": case "tar": case "gz": case "xz": case "bz2": return ARCHIVE;
        case "txt": case "md": case "pdf": case "doc": case "docx": case "xls": case "xlsx": case "ppt": case "pptx":
        case "epub": case "srt": case "ass": case "csv": case "json": case "log": return DOC;
        default: return FILE;
        }
    }
    static boolean isImage(String name) { return forFile(name, false) == IMAGE; }
}
