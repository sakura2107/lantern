package org.lantern.client;
import java.io.*;
import java.net.*;
import java.util.*;
/** Non-secret endpoint metadata; wire constants match network.hpp. */
public final class ConnectionEndpoint {
    public static final int LAN_V4 = 1, LAN_V6 = 2, PUBLIC_V6 = 3, HOSTNAME = 4, PUBLIC_V4 = 5;
    public final String host;
    public final int kind, port, priority, failures;
    public final long expiresAt;
    public ConnectionEndpoint(int kind, String host, int port, int priority, long expires, int failures) {
        this.kind = kind;
        this.host = normalize(host);
        this.port = port;
        this.priority = priority;
        expiresAt = expires;
        this.failures = failures;
    }
    public boolean publicPath() {
        return kind >= 3;
    }
    static byte[] numeric(String text) {
        if (text == null)
            return null;
        if (text.indexOf(':') < 0) {
            String[] p = text.split("\\.", -1);
            if (p.length != 4)
                return null;
            byte[] b = new byte[4];
            for (int i = 0; i < 4; i++) {
                if (!p[i].matches("0|[1-9][0-9]{0,2}"))
                    return null;
                int n = Integer.parseInt(p[i]);
                if (n > 255)
                    return null;
                b[i] = (byte)n;
            }
            return b;
        }
        if (text.length() > 45 || !text.matches("[0-9a-fA-F:.]+"))
            return null;
        if (text.indexOf('.') >= 0 && numeric(text.substring(text.lastIndexOf(':') + 1)) == null)
            return null;
        // Only literal characters reach InetAddress: this path can never issue a DNS query.
        try {
            return InetAddress.getByName(text).getAddress();
        } catch (Exception ignored) {
            return null;
        }
    }
    static String normalize(String s) {
        byte[] b = numeric(s);
        if (b == null)
            return s == null ? "" : s.toLowerCase(Locale.ROOT);
        if (b.length == 4)
            return (b[0] & 255) + "." + (b[1] & 255) + "." + (b[2] & 255) + "." + (b[3] & 255);
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < 16; i += 2) {
            if (i > 0)
                out.append(':');
            out.append(Integer.toHexString((b[i] & 255) * 256 + (b[i + 1] & 255)));
        }
        return out.toString();
    }
    static boolean hostname(String s) {
        if (s == null || s.length() > 253 || s.indexOf('.') < 0 || numeric(s) != null || s.matches("[0-9.]+"))
            return false;
        for (String p : s.split("\\.", -1))
            if (p.isEmpty() || p.length() > 63 || !p.matches("[a-zA-Z0-9](?:[a-zA-Z0-9-]*[a-zA-Z0-9])?"))
                return false;
        return true;
    }
    static boolean validHost(String s) {
        if (hostname(s))
            return true;
        byte[] b = numeric(s);
        if (b == null)
            return false;
        if (b.length == 4)
            return (b[0] & 255) > 0 && (b[0] & 255) < 224 && !((b[0] & 255) == 169 && (b[1] & 255) == 254);
        int first = b[0] & 255;
        boolean zero = true;
        for (int i = 0; i < 15; i++)
            if (b[i] != 0)
                zero = false;
        return (zero && b[15] == 1) || (first & 254) == 252 || (first & 224) == 32;
    }
    public static ConnectionEndpoint of(String host, int port) {
        byte[] b = numeric(host);
        int kind = LAN_V4;
        if (b == null)
            kind = HOSTNAME;
        else if (b.length == 16)
            kind = (b[0] & 254) == 252 ? LAN_V6 : (b[0] == 0 ? LAN_V4 : PUBLIC_V6);
        else {
            int x = b[0] & 255, y = b[1] & 255;
            if (!(x == 10 || x == 127 || (x == 192 && y == 168) || (x == 172 && (y & 240) == 16)))
                kind = PUBLIC_V4;
        }
        return new ConnectionEndpoint(kind, host, port, kind * 100, 0, 0);
    }
    static List<ConnectionEndpoint> merge(List<ConnectionEndpoint> saved, List<ConnectionEndpoint> fresh,
                                          long now) {
        List<ConnectionEndpoint> out = new ArrayList<>();
        List<ConnectionEndpoint> all = new ArrayList<>(fresh);
        all.addAll(saved);
        for (ConnectionEndpoint e : all) {
            if (!validHost(e.host) || e.port < 1 || e.port > 65535 || e.kind < 1 || e.kind > 5)
                continue;
            if (of(e.host, e.port).kind != e.kind)
                continue;
            if (e.kind != HOSTNAME && ((e.expiresAt != 0 && e.expiresAt <= now) || e.failures >= 3))
                continue;
            boolean exists = false;
            for (ConnectionEndpoint old : out)
                if (old.host.equals(e.host) && old.port == e.port)
                    exists = true;
            if (!exists)
                out.add(e);
        }
        out.sort(Comparator.<ConnectionEndpoint>comparingInt(e -> e.publicPath() ? 1 : 0)
                     .thenComparingInt(e -> e.priority));
        while (out.size() > 8) {
            int remove = out.size() - 1;
            while (remove >= 0 && out.get(remove).kind == HOSTNAME)
                remove--;
            out.remove(remove < 0 ? out.size() - 1 : remove);
        }
        return Collections.unmodifiableList(out);
    }
    static List<ConnectionEndpoint> recordResults(List<ConnectionEndpoint> saved,
                                                  List<NetworkDialer.Probe> reports) {
        List<ConnectionEndpoint> out = new ArrayList<>();
        for (ConnectionEndpoint e : saved) {
            int failures = e.failures;
            for (NetworkDialer.Probe p : reports)
                if (e.host.equals(p.endpoint.host) && e.port == p.endpoint.port) {
                    if (p.failure == null)
                        failures = 0;
                    else if (p.failure != NetworkDialer.Failure.CANCELLED &&
                             p.failure != NetworkDialer.Failure.PIN_MISMATCH)
                        failures = Math.min(100, failures + 1);
                }
            out.add(new ConnectionEndpoint(e.kind, e.host, e.port, e.priority, e.expiresAt, failures));
        }
        return merge(Collections.emptyList(), out, System.currentTimeMillis() / 1000);
    }
    static List<ConnectionEndpoint> read(DataInputStream r) throws IOException {
        int n = r.readInt();
        if (n < 0 || n > 8)
            throw new IOException("入口数量无效");
        List<ConnectionEndpoint> out = new ArrayList<>();
        for (int i = 0; i < n; i++) {
            int k = r.readInt();
            String h = Remote.str(r);
            int p = r.readInt(), priority = r.readInt();
            long expiry = r.readLong();
            int failures = r.readInt();
            if (!validHost(h) || h.length() > 253 || p < 1 || p > 65535 || k < 1 || k > 5 ||
                of(h, p).kind != k || failures < 0 || expiry < 0)
                throw new IOException("入口字段无效");
            out.add(new ConnectionEndpoint(k, h, p, priority, expiry, failures));
        }
        return out;
    }
    /** Versioned text for SharedPreferences; no tokens or certificate material. */
    public static String savePreferences(List<ConnectionEndpoint> list) {
        StringBuilder out = new StringBuilder("1");
        for (ConnectionEndpoint e : merge(Collections.emptyList(), list, System.currentTimeMillis() / 1000))
            out.append('\n')
                .append(e.kind)
                .append(',')
                .append(e.host)
                .append(',')
                .append(e.port)
                .append(',')
                .append(e.priority)
                .append(',')
                .append(e.expiresAt)
                .append(',')
                .append(e.failures);
        return out.toString();
    }
    public static List<ConnectionEndpoint> loadPreferences(String value) {
        try {
            if (value == null || value.length() > 8192)
                return Collections.emptyList();
            String[] lines = value.split("\n", -1);
            if (!lines[0].equals("1") || lines.length > 9)
                return Collections.emptyList();
            List<ConnectionEndpoint> out = new ArrayList<>();
            for (int i = 1; i < lines.length; i++) {
                String[] p = lines[i].split(",", -1);
                if (p.length != 6)
                    return Collections.emptyList();
                out.add(new ConnectionEndpoint(Integer.parseInt(p[0]), p[1], Integer.parseInt(p[2]),
                                               Integer.parseInt(p[3]), Long.parseLong(p[4]),
                                               Integer.parseInt(p[5])));
            }
            return merge(Collections.emptyList(), out, System.currentTimeMillis() / 1000);
        } catch (Exception ignored) {
            return Collections.emptyList();
        }
    }
}
