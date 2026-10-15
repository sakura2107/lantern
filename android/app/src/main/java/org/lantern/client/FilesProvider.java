package org.lantern.client;
import android.content.*;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.*;
import android.os.storage.StorageManager;
import android.provider.OpenableColumns;
import android.system.*;
import android.webkit.MimeTypeMap;
import java.io.*;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

public final class FilesProvider extends ContentProvider {
    static final class Source {
        final File file;
        final Remote.Credentials credentials;
        final String path, name;
        final Remote.Info info;
        Source(File f) {
            file = f;
            name = f.getName();
            credentials = null;
            path = null;
            info = null;
        }
        Source(Remote.Credentials c, String p, Remote.Info i) {
            file = null;
            credentials = c;
            path = p;
            name = p.substring(p.lastIndexOf('/') + 1);
            info = i;
        }
        long size() {
            return file != null ? file.length() : info.size;
        }
    }
    static final ConcurrentHashMap<String, Source> sources = new ConcurrentHashMap<>();
    static final AtomicInteger activeProxies = new AtomicInteger();
    static Uri share(Source source) throws IOException {
        if (sources.size() >= 32)
            throw new IOException("共享句柄已达上限，请重启应用结束旧共享");
        String id = UUID.randomUUID().toString();
        sources.put(id, source);
        return new Uri.Builder()
            .scheme("content")
            .authority("org.lantern.client.files")
            .appendPath(id)
            .appendPath(source.name)
            .build();
    }
    static Source source(Uri uri) throws FileNotFoundException {
        List<String> p = uri.getPathSegments();
        if (p.size() != 2)
            throw new FileNotFoundException("Invalid URI");
        Source s = sources.get(p.get(0));
        if (s == null || !p.get(1).equals(s.name))
            throw new FileNotFoundException("共享已结束，请重新打开");
        return s;
    }
    static String mime(String name) {
        int dot = name.lastIndexOf('.');
        String ext = dot < 0 ? "" : name.substring(dot + 1).toLowerCase(Locale.ROOT);
        if (ext.equals("mkv"))
            return "video/x-matroska";
        String m = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        return m == null ? "application/octet-stream" : m;
    }
    @Override
    public boolean onCreate() {
        return true;
    }
    @Override
    public String getType(Uri uri) {
        try {
            return mime(source(uri).name);
        } catch (Exception e) {
            return "application/octet-stream";
        }
    }
    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        try {
            Source s = source(uri);
            MatrixCursor c =
                new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
            c.addRow(new Object[] {s.name, s.size()});
            return c;
        } catch (Exception e) {
            return null;
        }
    }
    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode))
            throw new FileNotFoundException("Read-only sharing");
        Source s = source(uri);
        if (s.file != null) {
            try {
                File base = new File(getContext().getFilesDir(), "received").getCanonicalFile();
                File actual = s.file.getCanonicalFile();
                File transfers=new File(getContext().getFilesDir(),"transfers").getCanonicalFile();
                if (!actual.toPath().startsWith(base.toPath())&&!actual.toPath().startsWith(transfers.toPath()))
                    throw new FileNotFoundException("Outside cache");
                return ParcelFileDescriptor.open(actual, ParcelFileDescriptor.MODE_READ_ONLY);
            } catch (IOException e) {
                throw new FileNotFoundException(e.getMessage());
            }
        }
        if (activeProxies.incrementAndGet() > 8) {
            activeProxies.decrementAndGet();
            throw new FileNotFoundException("可寻址读取最多 8 个活动文件");
        }
        TransferPacer.SHARED.movie=true;
        HandlerThread thread = new HandlerThread("lantern-media-read");
        thread.start();
        StorageManager storage = getContext().getSystemService(StorageManager.class);
        try {
            return storage.openProxyFileDescriptor(
                ParcelFileDescriptor.MODE_READ_ONLY, new ProxyFileDescriptorCallback() {
                    RemoteFileReader reader;
                    @Override
                    public long onGetSize() {
                        return s.info.size;
                    }
                    @Override
                    public int onRead(long offset, int size, byte[] data) throws ErrnoException {
                        try {
                            if (offset < 0 || size < 0)
                                throw new IOException("Range");
                            if(reader==null){reader=new RemoteFileReader(s.credentials,s.path);if(reader.info.size!=s.info.size||!reader.info.version.equals(s.info.version)){reader.close();reader=null;throw new IOException("Source changed");}}
                            int total=(int)Math.min(Math.min(size,data.length),Math.max(0,s.info.size-offset)),done=0;
                            while(done<total){int count=reader.readAt(offset+done,data,done,total-done);if(count<0)break;if(count==0)throw new IOException("Read made no progress");done+=count;}
                            return done;
                        } catch (Exception e) {
                            throw new ErrnoException("Remote source unavailable or changed", OsConstants.EIO);
                        }
                    }
                    @Override
                    public void onRelease() {
                        try {
                            if (reader != null)reader.close();
                        } catch (Exception ignored) {
                        }
                        TransferPacer.SHARED.movie=activeProxies.decrementAndGet()>0;
                        thread.quitSafely();
                    }
                }, new Handler(thread.getLooper()));
        } catch (IOException e) {
            TransferPacer.SHARED.movie=activeProxies.decrementAndGet()>0;
            thread.quitSafely();
            throw new FileNotFoundException("系统不支持可寻址读取，请下载后打开：" + e.getMessage());
        }
    }
    @Override
    public Uri insert(Uri u, ContentValues v) {
        throw new UnsupportedOperationException("Read only");
    }
    @Override
    public int delete(Uri u, String s, String[] a) {
        throw new UnsupportedOperationException("Read only");
    }
    @Override
    public int update(Uri u, ContentValues v, String s, String[] a) {
        throw new UnsupportedOperationException("Read only");
    }
}
