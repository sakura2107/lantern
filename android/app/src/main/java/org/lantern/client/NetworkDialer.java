package org.lantern.client;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.security.cert.*;
import java.util.*;
import java.util.concurrent.*;
import javax.net.ssl.*;

/** Worker-thread API. Endpoints and DNS address families race at 250ms intervals. */
final class NetworkDialer {
    enum Failure { DNS, TIMEOUT, REFUSED, PIN_MISMATCH, TRANSPORT, CANCELLED }
    static final class Probe {
        final ConnectionEndpoint endpoint;
        final Failure failure;
        final long elapsedMs, startMs;
        final boolean tcpOk, tlsOk;
        final Long rttMs;
        Probe(ConnectionEndpoint e, Failure f, long elapsed, long start, boolean tcp, boolean tls, Long rtt) {
            endpoint = e;
            failure = f;
            elapsedMs = elapsed;
            startMs = start;
            tcpOk = tcp;
            tlsOk = tls;
            rttMs = rtt;
        }
    }
    static final class ConnectException extends IOException {
        final Failure reason;
        final List<Probe> attempts;
        ConnectException(Failure r, List<Probe> a) {
            super(r == Failure.PIN_MISMATCH ? "主机证书指纹不匹配" : "所有连接入口均不可用");
            reason = r;
            attempts = a;
        }
    }
    static final class PinException extends CertificateException {}
    private static ThreadFactory daemon(String name) {
        return r -> {
            Thread t = new Thread(r, name);
            t.setDaemon(true);
            return t;
        };
    }
    // Android system DNS is not always interruptible. Its bounded pool cannot delay the caller deadline.
    private static final ExecutorService resolvers =
        new ThreadPoolExecutor(4, 4, 30, TimeUnit.SECONDS, new ArrayBlockingQueue<Runnable>(32),
                               daemon("Lantern-DNS"), new ThreadPoolExecutor.AbortPolicy());
    private static final ExecutorService attempts =
        new ThreadPoolExecutor(16, 16, 30, TimeUnit.SECONDS, new ArrayBlockingQueue<Runnable>(128),
                               daemon("Lantern-connect"), new ThreadPoolExecutor.AbortPolicy());
    private static final ScheduledExecutorService scheduler =
        Executors.newSingleThreadScheduledExecutor(daemon("Lantern-stagger"));
    private static long ms() {
        return System.nanoTime() / 1000000;
    }
    static void closeSocket(Socket s) {
        if (s != null)
            try {
                s.close();
            } catch (IOException ignored) {
            }
    }
    static final class Group extends Socket {
        final List<Socket> sockets = new ArrayList<>();
        volatile boolean cancelled = false;
        boolean settled = false;
        synchronized boolean add(Socket s) {
            if (cancelled || settled) {
                closeSocket(s);
                return false;
            }
            sockets.add(s);
            return true;
        }
        synchronized void remove(Socket s) {
            sockets.remove(s);
        }
        synchronized void keep(Socket winner) {
            settled = true;
            for (Socket s : new ArrayList<>(sockets))
                if (s != winner) {
                    closeSocket(s);
                    sockets.remove(s);
                }
        }
        @Override
        public synchronized void close() {
            cancelled = true;
            keep(null);
            notifyAll();
        }
    }
    static final class Result {
        final SSLSocket tls;
        final Socket transport;
        final ConnectionEndpoint selected;
        final List<Probe> attempts;
        final boolean publicPath;
        Result(SSLSocket t, Socket raw, ConnectionEndpoint e, List<Probe> a) {
            tls = t;
            transport = raw;
            selected = e;
            attempts = a;
            publicPath = e.publicPath();
        }
    }
    private static SSLContext context(String pin) throws Exception {
        if (pin == null || !pin.matches("[0-9a-f]{64}"))
            throw new IOException("主机指纹无效");
        X509TrustManager verify = new X509TrustManager() {
            public X509Certificate[] getAcceptedIssuers() {
                return new X509Certificate[0];
            }
            public void checkClientTrusted(X509Certificate[] c, String a) throws CertificateException {
                throw new CertificateException();
            }
            public void checkServerTrusted(X509Certificate[] chain, String a) throws CertificateException {
                try {
                    if (chain.length == 0)
                        throw new PinException();
                    chain[0].checkValidity();
                    String actual =
                        Remote.hex(MessageDigest.getInstance("SHA-256").digest(chain[0].getEncoded()));
                    if (!MessageDigest.isEqual(actual.getBytes(StandardCharsets.US_ASCII),
                                               pin.getBytes(StandardCharsets.US_ASCII)))
                        throw new PinException();
                } catch (Exception ignored) {
                    throw new PinException();
                }
            }
        };
        SSLContext c = SSLContext.getInstance("TLSv1.2");
        c.init(null, new TrustManager[] {verify}, new SecureRandom());
        return c;
    }
    static final class State {
        final ConnectionEndpoint endpoint;
        int pending = 0;
        long start = -1, end = 0;
        boolean done = false, tcp = false, tls = false;
        Failure failure = Failure.TIMEOUT;
        State(ConnectionEndpoint e) {
            endpoint = e;
        }
    }
    static final class Race {
        final Object lock = new Object();
        final Group group = new Group();
        final List<State> states = new ArrayList<>();
        final List<Future<?>> jobs = new ArrayList<>();
        final long begin = ms(), deadline;
        final SSLContext context;
        boolean finished = false, pinFailed = false;
        SSLSocket winner;
        Socket winningRaw;
        ConnectionEndpoint selected;
        Race(List<ConnectionEndpoint> endpoints, SSLContext c, int timeout) {
            context = c;
            deadline = begin + timeout;
            for (ConnectionEndpoint e : endpoints)
                states.add(new State(e));
        }
        boolean active() {
            synchronized (lock) {
                return !finished && ms() < deadline && !group.cancelled;
            }
        }
        void job(Future<?> f) {
            synchronized (lock) {
                if (finished)
                    f.cancel(true);
                else
                    jobs.add(f);
            }
        }
        void finish(State state, Failure failure, SSLSocket tls, Socket raw) {
            boolean retained = false;
            synchronized (lock) {
                if (!finished) {
                    state.pending--;
                    if (!state.done)
                        state.failure = failure;
                    if (failure == Failure.PIN_MISMATCH)
                        pinFailed = true;
                    if (tls != null && winner == null) {
                        winner = tls;
                        winningRaw = raw;
                        selected = state.endpoint;
                        retained = true;
                        state.tls = true;
                        state.done = true;
                    }
                    if (state.pending <= 0 || pinFailed)
                        state.done = true;
                    if (state.done)
                        state.end = ms();
                    lock.notifyAll();
                }
            }
            if (!retained) {
                group.remove(raw);
                closeSocket(raw);
            }
        }
        void connect(State state, InetAddress address) {
            if (!active())
                return;
            Socket raw = new Socket();
            if (!group.add(raw))
                return;
            SSLSocket tls = null;
            Failure failure = Failure.TRANSPORT;
            try {
                synchronized (lock) {
                    if (state.start < 0)
                        state.start = ms();
                }
                raw.connect(new InetSocketAddress(address, state.endpoint.port),
                            (int)Math.max(1, deadline - ms()));
                raw.setTcpNoDelay(true);
                synchronized (lock) {
                    state.tcp = true;
                }
                tls = (SSLSocket)context.getSocketFactory().createSocket(raw, state.endpoint.host,
                                                                         state.endpoint.port, true);
                tls.setEnabledProtocols(new String[] {"TLSv1.2"});
                tls.setSoTimeout((int)Math.max(1, deadline - ms()));
                tls.startHandshake();
                failure = null;
            } catch (Exception error) {
                if (error instanceof SocketTimeoutException)
                    failure = Failure.TIMEOUT;
                else if (error instanceof java.net.ConnectException)
                    failure = Failure.REFUSED;
                for (Throwable cause = error; cause != null; cause = cause.getCause())
                    if (cause instanceof PinException)
                        failure = Failure.PIN_MISMATCH;
                tls = null;
            } finally {
                finish(state, failure, tls, raw);
            }
        }
        void resolve(State state) {
            if (!active())
                return;
            try {
                InetAddress[] found = InetAddress.getAllByName(state.endpoint.host);
                List<InetAddress> ordered = new ArrayList<>(), v4 = new ArrayList<>(), v6 = new ArrayList<>();
                for (InetAddress a : found)
                    (a instanceof Inet6Address ? v6 : v4).add(a);
                for (int i = 0; ordered.size() < 8 && (i < v4.size() || i < v6.size()); i++) {
                    if (i < v6.size())
                        ordered.add(v6.get(i));
                    if (i < v4.size() && ordered.size() < 8)
                        ordered.add(v4.get(i));
                }
                synchronized (lock) {
                    if (finished)
                        return;
                    state.pending = ordered.size();
                    if (state.start < 0)
                        state.start = ms();
                }
                if (ordered.isEmpty()) {
                    finish(state, Failure.DNS, null, null);
                    return;
                }
                for (int i = 0; i < ordered.size(); i++) {
                    InetAddress a = ordered.get(i);
                    job(scheduler.schedule(() -> {
                        if (!active())
                            return;
                        try {
                            job(attempts.submit(() -> connect(state, a)));
                        } catch (RejectedExecutionException e) {
                            finish(state, Failure.TRANSPORT, null, null);
                        }
                    }, 250L * i, TimeUnit.MILLISECONDS));
                }
            } catch (UnknownHostException error) {
                finish(state, Failure.DNS, null, null);
            }
        }
        List<Probe> reports() {
            List<Probe> out = new ArrayList<>();
            for (State s : states) {
                long start = s.start < 0 ? begin : s.start;
                Failure failure = s.done ? s.failure : Failure.CANCELLED;
                if (!s.done && winner == null && !group.cancelled)
                    failure = Failure.TIMEOUT;
                out.add(new Probe(s.endpoint, failure, Math.max(0, (s.end == 0 ? ms() : s.end) - start),
                                  Math.max(0, start - begin), s.tcp, s.tls, null));
            }
            return out;
        }
    }
    static Result connect(List<ConnectionEndpoint> candidates, String pin, int timeout,
                          Remote.SocketReady opening) throws Exception {
        return connect(candidates, pin, timeout, opening, false);
    }
    static Result connectLegacy(String host, int port, String pin, int timeout, Remote.SocketReady opening)
        throws Exception {
        return connect(Collections.singletonList(ConnectionEndpoint.of(host, port)), pin, timeout, opening,
                       true);
    }
    private static Result connect(List<ConnectionEndpoint> candidates, String pin, int timeout,
                                  Remote.SocketReady opening, boolean legacy) throws Exception {
        if (timeout < 1 || timeout > 60000 || candidates.isEmpty() || candidates.size() > 8)
            throw new IOException("连接参数无效");
        List<ConnectionEndpoint> list = legacy ? candidates
                                               : ConnectionEndpoint.merge(Collections.emptyList(), candidates,
                                                                          System.currentTimeMillis() / 1000);
        Race race = new Race(list, context(pin), timeout);
        if (opening != null)
            opening.opened(race.group);
        for (int i = 0; i < race.states.size(); i++) {
            State state = race.states.get(i);
            race.job(scheduler.schedule(() -> {
                if (!race.active())
                    return;
                try {
                    race.job(resolvers.submit(() -> race.resolve(state)));
                } catch (RejectedExecutionException e) {
                    race.finish(state, Failure.TRANSPORT, null, null);
                }
            }, 250L * i, TimeUnit.MILLISECONDS));
        }
        try {
            synchronized (race.lock) {
                while (race.winner == null && !race.pinFailed &&
                       race.states.stream().anyMatch(s -> !s.done) && ms() < race.deadline &&
                       !race.group.cancelled)
                    race.lock.wait(Math.max(1, Math.min(50, race.deadline - ms())));
                race.finished = true;
                List<Probe> reports = race.reports();
                if (race.winner == null || race.pinFailed) {
                    Failure reason = race.pinFailed         ? Failure.PIN_MISMATCH
                                     : race.group.cancelled ? Failure.CANCELLED
                                                            : Failure.TIMEOUT;
                    race.group.close();
                    throw new ConnectException(reason, reports);
                }
                race.group.keep(race.winningRaw);
                return new Result(race.winner, race.group, race.selected, reports);
            }
        } catch (InterruptedException error) {
            race.group.close();
            throw error;
        } finally {
            synchronized (race.lock) {
                race.finished = true;
                for (Future<?> job : race.jobs)
                    if (!job.isDone())
                        job.cancel(true);
            }
        }
    }
}
