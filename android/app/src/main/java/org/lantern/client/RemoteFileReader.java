package org.lantern.client;

import java.io.*;
import java.util.*;

/** Random access to the original version, with a bounded 2 MiB LRU. No desktop frames. */
final class RemoteFileReader implements AutoCloseable {
    final Remote.Credentials credentials;
    final String path;
    final Remote.Info info;
    final LinkedHashMap<Long, byte[]> cache = new LinkedHashMap<>(8, 0.75f, true);
    volatile boolean closed;
    volatile Remote connection;
    volatile java.net.Socket opening;
    volatile long transferred, requests, hits, reconnects;
    RemoteFileReader(Remote.Credentials c, String p) throws Exception {
        credentials = c;
        path = p;
        connection = open();
        try {
            info = connection.stat(path, false);
        } catch (Exception error) {
            connection.abort();
            throw error;
        }
    }
    Remote open() throws Exception {
        return new Remote(credentials, 1, 2, s -> {
            opening = s;
            if (closed)
                s.close();
        });
    }
    byte[] block(long position) throws Exception {
        byte[] bytes = cache.get(position);
        if (bytes != null) {
            hits++;
            return bytes;
        }
        for (int attempt = 0;; attempt++) {
            if (closed)
                throw new IOException("播放已结束");
            try {
                if (connection == null) {
                    connection = open();
                    Remote.Info current = connection.stat(path, false);
                    if (!current.version.equals(info.version) || current.size != info.size)
                        throw new Remote.RemoteError(10, "媒体源文件已变化，请重新打开");
                    reconnects++;
                }
                bytes = connection.read(path, info.version, position,
                                        (int)Math.min(Remote.CHUNK, info.size - position));
                requests++;
                transferred += bytes.length;
                if (bytes.length != Math.min(Remote.CHUNK, info.size - position))
                    throw new IOException("媒体文件提前结束");
                break;
            } catch (Remote.RemoteError | javax.net.ssl.SSLException error) {
                throw error;
            } catch (IOException error) {
                if (connection != null)
                    try {
                        connection.abort();
                    } catch (Exception ignored) {
                    }
                connection = null;
                if (attempt >= 2 || closed)
                    throw error;
                Thread.sleep(100L * (attempt + 1));
            }
        }
        if (cache.size() == 8)
            cache.remove(cache.keySet().iterator().next());
        cache.put(position, bytes);
        return bytes;
    }
    public synchronized int readAt(long position, byte[] buffer, int offset, int size) throws IOException {
        if (position < 0 || offset < 0 || size < 0 || offset > buffer.length - size)
            throw new IOException("媒体读取范围无效");
        if (size == 0)
            return 0;
        if (position >= info.size)
            return -1;
        try {
            long start = position / Remote.CHUNK * Remote.CHUNK;
            byte[] bytes = block(start);
            int at = (int)(position - start);
            int count = Math.min(size, bytes.length - at);
            System.arraycopy(bytes, at, buffer, offset, count);
            return count;
        } catch (Exception error) {
            throw error instanceof IOException ? (IOException)error : new IOException(error);
        }
    }
    public long getSize() {
        return info.size;
    }
    // Never wait for readAt's monitor: cancellation must interrupt a blocked socket immediately.
    public void close() {
        closed = true;
        try {
            if (opening != null)
                opening.close();
        } catch (IOException ignored) {
        }
        try {
            if (connection != null)
                connection.abort();
        } catch (IOException ignored) {
        }
    }
}
