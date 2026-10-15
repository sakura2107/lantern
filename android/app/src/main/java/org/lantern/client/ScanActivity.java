package org.lantern.client;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.*;
import android.hardware.camera2.*;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Size;
import android.util.TypedValue;
import android.view.*;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import com.google.zxing.BarcodeFormat;
import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.PlanarYUVLuminanceSource;
import com.google.zxing.ReaderException;
import com.google.zxing.Result;
import com.google.zxing.common.HybridBinarizer;
import com.google.zxing.qrcode.QRCodeReader;
import java.nio.ByteBuffer;
import java.util.*;

/**
 * Full-screen QR scanner for pairing (Camera2 + ZXing core, no other libraries). Returns only a
 * lantern://pair link; anything else is reported and ignored. Frames stay in memory and are never stored.
 */
public final class ScanActivity extends Activity implements TextureView.SurfaceTextureListener {
    static final String EXTRA_LINK = "link";
    static final String PREFIX = "lantern://pair?";
    private static final int MAX_AREA = 1280 * 960;

    private TextureView preview;
    private Overlay overlay;
    private HandlerThread thread;
    private Handler handler;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private ImageReader reader;
    private Size size;
    private int sensorOrientation = 90;
    private volatile boolean finished;
    private long lastHint;
    private byte[] luma;
    private final QRCodeReader qr = new QRCodeReader();
    private final Map<DecodeHintType, Object> hints = new EnumMap<>(DecodeHintType.class);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        hints.put(DecodeHintType.TRY_HARDER, Boolean.TRUE);
        hints.put(DecodeHintType.POSSIBLE_FORMATS, Collections.singletonList(BarcodeFormat.QR_CODE));
        hints.put(DecodeHintType.CHARACTER_SET, "UTF-8");
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        w.setStatusBarColor(Color.BLACK);
        w.setNavigationBarColor(Color.BLACK);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        preview = new TextureView(this);
        preview.setSurfaceTextureListener(this);
        root.addView(preview, new FrameLayout.LayoutParams(-1, -1));
        overlay = new Overlay(this);
        root.addView(overlay, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout actions = Ui.row(this);
        actions.setGravity(Gravity.CENTER);
        Button cancel = Ui.button(this, S.CANCEL_SCAN, Ui.SECONDARY, this::finish);
        Ui.id(cancel, "scan.cancel");
        actions.addView(cancel, new LinearLayout.LayoutParams(0, Theme.dp(48), 1));
        Button paste = Ui.button(this, S.PASTE_PAIR_LINK, Ui.PRIMARY, this::paste);
        Ui.withIcon(paste, Glyph.CLIPBOARD);
        Ui.id(paste, "scan.paste");
        LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(0, Theme.dp(48), 1);
        pp.setMarginStart(Theme.dp(12));
        actions.addView(paste, pp);
        FrameLayout.LayoutParams ap = new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM);
        ap.setMargins(Theme.dp(24), 0, Theme.dp(24), Theme.dp(36));
        root.addView(actions, ap);
        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        thread = new HandlerThread("lantern-scan");
        thread.start();
        handler = new Handler(thread.getLooper());
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {Manifest.permission.CAMERA}, 1);
        else if (preview.isAvailable())
            open();
    }

    @Override
    protected void onPause() {
        close();
        if (thread != null) {
            thread.quitSafely();
            try {
                thread.join(500);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            thread = null;
            handler = null;
        }
        super.onPause();
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
            if (preview.isAvailable())
                open();
        } else
            overlay.message(S.CAMERA_DENIED, 0);
    }

    @Override public void onSurfaceTextureAvailable(SurfaceTexture t, int w, int h) {
        if (handler != null && checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED)
            open();
    }
    @Override public void onSurfaceTextureSizeChanged(SurfaceTexture t, int w, int h) { transform(); }
    @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture t) { return true; }
    @Override public void onSurfaceTextureUpdated(SurfaceTexture t) {}

    private void open() {
        if (camera != null || handler == null)
            return;
        try {
            CameraManager manager = getSystemService(CameraManager.class);
            String id = null;
            for (String candidate : manager.getCameraIdList()) {
                Integer facing = manager.getCameraCharacteristics(candidate).get(CameraCharacteristics.LENS_FACING);
                if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) {
                    id = candidate;
                    break;
                }
                if (id == null)
                    id = candidate;
            }
            if (id == null) {
                overlay.message(S.CAMERA_NONE, 0);
                return;
            }
            CameraCharacteristics info = manager.getCameraCharacteristics(id);
            Integer orientation = info.get(CameraCharacteristics.SENSOR_ORIENTATION);
            sensorOrientation = orientation == null ? 90 : orientation;
            StreamConfigurationMap map = info.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            size = pick(map.getOutputSizes(ImageFormat.YUV_420_888));
            reader = ImageReader.newInstance(size.getWidth(), size.getHeight(), ImageFormat.YUV_420_888, 2);
            reader.setOnImageAvailableListener(this::frame, handler);
            transform();
            manager.openCamera(id, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) {
                    camera = d;
                    start();
                }
                @Override public void onDisconnected(CameraDevice d) {
                    d.close();
                    camera = null;
                }
                @Override public void onError(CameraDevice d, int error) {
                    d.close();
                    camera = null;
                    overlay.message(S.CAMERA_FAILED, 0);
                }
            }, handler);
        } catch (CameraAccessException | SecurityException | IllegalArgumentException e) {
            overlay.message(S.CAMERA_FAILED, 0);
        }
    }

    /** Largest stream no bigger than ~1.2 MP: enough detail for a dense QR, cheap to decode. */
    private static Size pick(Size[] sizes) {
        Size best = sizes[0];
        long bestArea = -1;
        for (Size s : sizes) {
            long area = (long)s.getWidth() * s.getHeight();
            if (area <= MAX_AREA && area > bestArea) {
                best = s;
                bestArea = area;
            }
        }
        return best;
    }

    private void start() {
        try {
            SurfaceTexture texture = preview.getSurfaceTexture();
            if (texture == null || camera == null)
                return;
            texture.setDefaultBufferSize(size.getWidth(), size.getHeight());
            Surface view = new Surface(texture);
            CaptureRequest.Builder request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            request.addTarget(view);
            request.addTarget(reader.getSurface());
            request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
            camera.createCaptureSession(Arrays.asList(view, reader.getSurface()), new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession s) {
                    session = s;
                    try {
                        s.setRepeatingRequest(request.build(), null, handler);
                    } catch (CameraAccessException | IllegalStateException e) {
                        overlay.message(S.CAMERA_FAILED, 0);
                    }
                }
                @Override public void onConfigureFailed(CameraCaptureSession s) { overlay.message(S.CAMERA_FAILED, 0); }
            }, handler);
        } catch (CameraAccessException | IllegalStateException e) {
            overlay.message(S.CAMERA_FAILED, 0);
        }
    }

    private void close() {
        try {
            if (session != null)
                session.close();
        } catch (RuntimeException ignored) {
        }
        session = null;
        if (camera != null)
            camera.close();
        camera = null;
        if (reader != null)
            reader.close();
        reader = null;
    }

    /** The camera thread: copy the luminance plane, release the frame, then try to decode the centre. */
    private void frame(ImageReader r) {
        Image image = r.acquireLatestImage();
        if (image == null)
            return;
        int w, h;
        try {
            if (finished)
                return;
            w = image.getWidth();
            h = image.getHeight();
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buffer = plane.getBuffer();
            int stride = plane.getRowStride();
            if (luma == null || luma.length != w * h)
                luma = new byte[w * h];
            if (stride == w)
                buffer.get(luma, 0, w * h);
            else
                for (int row = 0; row < h; row++) {
                    buffer.position(row * stride);
                    buffer.get(luma, row * w, w);
                }
        } finally {
            image.close();
        }
        int side = (int)(Math.min(w, h) * 0.85f);
        PlanarYUVLuminanceSource source =
            new PlanarYUVLuminanceSource(luma, w, h, (w - side) / 2, (h - side) / 2, side, side, false);
        try {
            Result result = qr.decode(new BinaryBitmap(new HybridBinarizer(source)), hints);
            found(result.getText());
        } catch (ReaderException ignored) {
            // No code in this frame.
        } finally {
            qr.reset();
        }
    }

    private void found(String text) {
        if (text != null && text.startsWith(PREFIX)) {
            finished = true;
            runOnUiThread(() -> {
                preview.performHapticFeedback(Build.VERSION.SDK_INT >= 30 ? HapticFeedbackConstants.CONFIRM
                                                                         : HapticFeedbackConstants.VIRTUAL_KEY);
                setResult(RESULT_OK, new Intent().putExtra(EXTRA_LINK, text));
                finish();
            });
            return;
        }
        long now = SystemClock.uptimeMillis();
        if (now - lastHint > 2500) {
            lastHint = now;
            overlay.message(S.SCAN_NOT_LANTERN, 2200);
        }
    }

    private void paste() {
        ClipboardManager clipboard = (ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE);
        ClipData clip = clipboard.getPrimaryClip();
        CharSequence text = clip == null || clip.getItemCount() == 0 ? null : clip.getItemAt(0).getText();
        if (text == null || !text.toString().trim().startsWith(PREFIX)) {
            overlay.message(S.CLIPBOARD_NOT_LINK, 2200);
            return;
        }
        finished = true;
        setResult(RESULT_OK, new Intent().putExtra(EXTRA_LINK, text.toString().trim()));
        finish();
    }

    /** TextureView stretches the camera buffer to the view; scale it back to a centre crop. */
    private void transform() {
        runOnUiThread(() -> {
            int vw = preview.getWidth(), vh = preview.getHeight();
            if (size == null || vw == 0 || vh == 0)
                return;
            boolean swap = sensorOrientation == 90 || sensorOrientation == 270;
            float bw = swap ? size.getHeight() : size.getWidth(), bh = swap ? size.getWidth() : size.getHeight();
            float scale = Math.max(vw / bw, vh / bh);
            Matrix m = new Matrix();
            m.setScale(bw * scale / vw, bh * scale / vh, vw / 2f, vh / 2f);
            preview.setTransform(m);
        });
    }

    /** Dims everything but the viewfinder, draws amber corners, a sweeping line and the hints. */
    static final class Overlay extends View {
        private final Paint dim = new Paint(), corner = new Paint(Paint.ANTI_ALIAS_FLAG), sweep = new Paint(),
                            title = new Paint(Paint.ANTI_ALIAS_FLAG), sub = new Paint(Paint.ANTI_ALIAS_FLAG),
                            pill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path hole = new Path();
        private final RectF box = new RectF();
        private String message;
        private long messageUntil;

        Overlay(Context c) {
            super(c);
            dim.setColor(0xA6000000);
            corner.setColor(Theme.LAMP);
            corner.setStyle(Paint.Style.STROKE);
            corner.setStrokeWidth(Theme.dp(4));
            corner.setStrokeCap(Paint.Cap.ROUND);
            title.setColor(Color.WHITE);
            title.setTextAlign(Paint.Align.CENTER);
            title.setTypeface(Theme.MEDIUM);
            title.setTextSize(sp(c, 20));
            sub.setColor(0xCCFFFFFF);
            sub.setTextAlign(Paint.Align.CENTER);
            sub.setTextSize(sp(c, 14));
            pill.setColor(0xD9000000);
        }
        static float sp(Context c, float v) {
            return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, v, c.getResources().getDisplayMetrics());
        }
        /** Shows a line under the viewfinder; 0 keeps it until replaced. Safe from any thread. */
        void message(String text, long millis) {
            post(() -> {
                message = text;
                messageUntil = millis == 0 ? Long.MAX_VALUE : SystemClock.uptimeMillis() + millis;
                invalidate();
            });
        }
        @Override protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            float side = Math.min(w, h) * 0.68f, cx = w / 2f, cy = h * 0.44f, r = Theme.dp(22);
            box.set(cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2);
            hole.reset();
            hole.addRoundRect(box, r, r, Path.Direction.CW);
            c.save();
            c.clipOutPath(hole);
            c.drawPaint(dim);
            c.restore();
            float l = side * 0.13f;
            Path p = new Path();
            p.moveTo(box.left, box.top + l); p.lineTo(box.left, box.top + r / 2); p.quadTo(box.left, box.top, box.left + r / 2, box.top); p.lineTo(box.left + l, box.top);
            p.moveTo(box.right - l, box.top); p.lineTo(box.right - r / 2, box.top); p.quadTo(box.right, box.top, box.right, box.top + r / 2); p.lineTo(box.right, box.top + l);
            p.moveTo(box.right, box.bottom - l); p.lineTo(box.right, box.bottom - r / 2); p.quadTo(box.right, box.bottom, box.right - r / 2, box.bottom); p.lineTo(box.right - l, box.bottom);
            p.moveTo(box.left + l, box.bottom); p.lineTo(box.left + r / 2, box.bottom); p.quadTo(box.left, box.bottom, box.left, box.bottom - r / 2); p.lineTo(box.left, box.bottom - l);
            c.drawPath(p, corner);
            // A soft amber line sweeping down and back up.
            float t = (SystemClock.uptimeMillis() % 2400) / 2400f, phase = t < .5f ? t * 2 : 2 - t * 2;
            float y = box.top + side * (0.08f + 0.84f * phase);
            sweep.setShader(new LinearGradient(box.left, 0, box.right, 0,
                new int[] {Color.TRANSPARENT, Theme.LAMP, Color.TRANSPARENT}, null, Shader.TileMode.CLAMP));
            sweep.setAlpha(200);
            c.drawRect(box.left + Theme.dp(16), y - Theme.dp(1), box.right - Theme.dp(16), y + Theme.dp(1), sweep);
            c.drawText(S.SCAN_TITLE, cx, box.top - Theme.dp(52), title);
            c.drawText(S.SCAN_SUBTITLE, cx, box.top - Theme.dp(24), sub);
            if (message != null && SystemClock.uptimeMillis() < messageUntil) {
                float tw = Math.min(w - Theme.dp(32), sub.measureText(message) + Theme.dp(32));
                RectF m = new RectF(cx - tw / 2, box.bottom + Theme.dp(24), cx + tw / 2, box.bottom + Theme.dp(64));
                c.drawRoundRect(m, m.height() / 2, m.height() / 2, pill);
                c.drawText(message, cx, m.centerY() - (sub.descent() + sub.ascent()) / 2, sub);
            }
            postInvalidateOnAnimation();
        }
    }
}
