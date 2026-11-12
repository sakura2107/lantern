package org.lantern.client;

import java.io.*;
import java.util.*;

/**
 * Random access to the original version for external players. While the player reads in order, up to
 * AHEAD blocks (16 MiB) after the current one are fetched by WORKERS extra connections, so with the
 * foreground read four requests are in flight. A jump elsewhere drops the queued read-ahead and starts
 * again from the new position. No desktop frames.
 */
final class RemoteFileReader implements AutoCloseable {
    // Blocks of Remote.CHUNK (256 KiB): 16 MiB ahead, 20 MiB kept per open file.
    static final int AHEAD = 64, KEEP = 80, WORKERS = 3;
    final Remote.Credentials credentials;
    final String path;
    final Remote.Info info;
    final Object lock = new Object();
    final LinkedHashMap<Long, byte[]> cache = new LinkedHashMap<>(KEEP, 0.75f, true); // under lock
    final Set<Long> loading = new HashSet<>();                                       // under lock
    final ArrayDeque<Long> queue = new ArrayDeque<>();                                // under lock
    final List<Thread> workers = new ArrayList<>();
    final List<Remote> extra = Collections.synchronizedList(new ArrayList<>());
    volatile boolean closed;
    volatile Remote connection;
    volatile java.net.Socket opening;
    volatile long transferred, requests, hits, reconnects;
    long lastBlock = -2; // foreground thread only
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
    int length(long position) {
        return (int)Math.min(Remote.CHUNK, info.size - position);
    }
    /** One block over the given connection holder, with the original two quiet retries. */
    byte[] fetch(long position, Remote[] holder) throws Exception {
        for (int attempt = 0;; attempt++) {
            if (closed)
                throw new IOException("播放已结束");
            try {
                if (holder[0] == null) {
                    holder[0] = open();
                    Remote.Info current = holder[0].stat(path, false);
                    if (!current.version.equals(info.version) || current.size != info.size)
                        throw new Remote.RemoteError(10, "媒体源文件已变化，请重新打开");
                    reconnects++;
                }
                byte[] bytes = holder[0].read(path, info.version, position, length(position));
                requests++;
                transferred += bytes.length;
                if (bytes.length != length(position))
                    throw new IOException("媒体文件提前结束");
                return bytes;
            } catch (Remote.RemoteError | javax.net.ssl.SSLException error) {
                throw error;
            } catch (IOException error) {
                if (holder[0] != null)
                    try {
                        holder[0].abort();
                    } catch (Exception ignored) {
                    }
                holder[0] = null;
                if (attempt >= 2 || closed)
                    throw error;
                Thread.sleep(100L * (attempt + 1));
            }
        }
    }
    void store(long position, byte[] bytes) {
        synchronized (lock) {
            loading.remove(position);
            if (bytes != null) {
                while (cache.size() >= KEEP)
                    cache.remove(cache.keySet().iterator().next());
                cache.put(position, bytes);
            }
            lock.notifyAll();
        }
    }
    /** Foreground: cached, already being fetched (wait for it), or fetched here on the main connection. */
    byte[] block(long position) throws Exception {
        synchronized (lock) {
            while (true) {
                byte[] bytes = cache.get(position);
                if (bytes != null) {
                    hits++;
                    return bytes;
                }
                if (!loading.contains(position))
                    break;
                if (closed)
                    throw new IOException("播放已结束");
                lock.wait(200);
            }
            queue.remove(position);
            loading.add(position);
        }
        byte[] bytes = null;
        try {
            Remote[] holder = {connection};
            try {
                bytes = fetch(position, holder);
            } finally {
                connection = holder[0];
            }
            return bytes;
        } finally {
            store(position, bytes);
        }
    }
    /** Queues the next AHEAD blocks; a jump elsewhere first drops what was queued for the old place. */
    void readAhead(long block) {
        boolean sequential = block == lastBlock || block == lastBlock + 1;
        lastBlock = block;
        long last = (info.size - 1) / Remote.CHUNK;
        synchronized (lock) {
            if (!sequential)
                queue.clear();
            for (long b = block + 1; b <= Math.min(last, block + AHEAD); b++) {
                long position = b * Remote.CHUNK;
                if (!cache.containsKey(position) && !loading.contains(position) && !queue.contains(position))
                    queue.addLast(position);
            }
            if (queue.isEmpty())
                return;
            while (workers.size() < WORKERS && !closed) {
                Thread t = new Thread(this::work, "lantern-read-ahead");
                t.setDaemon(true);
                workers.add(t);
                t.start();
            }
            lock.notifyAll();
        }
    }
    void work() {
        Remote[] holder = {null};
        try {
            while (!closed) {
                long position;
                synchronized (lock) {
                    while (queue.isEmpty() && !closed)
                        lock.wait(1000);
                    if (closed)
                        return;
                    position = queue.pollFirst();
                    loading.add(position);
                }
                byte[] bytes = null;
                try {
                    Remote before = holder[0];
                    bytes = fetch(position, holder);
                    if (holder[0] != before)
                        extra.add(holder[0]);
                } catch (Exception failed) {
                    // Read-ahead is optional: the foreground read fetches this block itself if needed.
                } finally {
                    store(position, bytes);
                }
            }
        } catch (InterruptedException stop) {
        } finally {
            if (holder[0] != null)
                try {
                    holder[0].abort();
                } catch (Exception ignored) {
                }
        }
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
            readAhead(start / Remote.CHUNK);
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
        synchronized (lock) {
            queue.clear();
            lock.notifyAll();
        }
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
        synchronized (extra) {
            for (Remote r : extra)
                try {
                    r.abort();
                } catch (IOException ignored) {
                }
        }
    }
}
