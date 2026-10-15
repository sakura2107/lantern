package org.lantern.client;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Strict offline QR parser. Malformed/expired input returns null and is never echoed. */
public final class PairingLink {
    public final String hostName, pin, code;
    public final List<String> addresses;
    public final int port;
    public final long expiresAt;
    private PairingLink(String name, List<String> addresses, int port, String pin, String code,
                        long expires) {
        hostName = name;
        this.addresses = Collections.unmodifiableList(addresses);
        this.port = port;
        this.pin = pin;
        this.code = code;
        expiresAt = expires;
    }
    static boolean validLabel(String text) {
        if (text == null || text.isEmpty())
            return false;
        for (int i = 0; i < text.length(); i++)
            if (text.charAt(i) < 32 || text.charAt(i) == 127)
                return false;
        try {
            return StandardCharsets.UTF_8.newEncoder()
                       .onMalformedInput(CodingErrorAction.REPORT)
                       .onUnmappableCharacter(CodingErrorAction.REPORT)
                       .encode(CharBuffer.wrap(text))
                       .remaining() <= 64;
        } catch (Exception ignored) {
            return false;
        }
    }
    static boolean validAddress(String address) {
        String[] parts = address.split("\\.", -1);
        if (parts.length != 4)
            return false;
        for (int i = 0; i < 4; i++) {
            if (!parts[i].matches("0|[1-9][0-9]{0,2}"))
                return false;
            int n = Integer.parseInt(parts[i]);
            if (n > 255 || (i == 0 && (n == 0 || n >= 224)))
                return false;
        }
        return true;
    }
    private static String decode(String text) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (int i = 0; i < text.length(); i++) {
            int c = text.charAt(i);
            if (c == '%') {
                if (i + 2 >= text.length())
                    throw new Exception();
                int a = hex(text.charAt(++i)), b = hex(text.charAt(++i));
                if (a < 0 || b < 0)
                    throw new Exception();
                c = a * 16 + b;
            } else if (c <= 32 || c >= 127 || c == '+' || c == '#')
                throw new Exception();
            if (c == 0)
                throw new Exception();
            out.write(c);
        }
        return StandardCharsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(out.toByteArray()))
            .toString();
    }
    private static int hex(char c) {
        return c >= '0' && c <= '9' ? c - '0'
        : c >= 'a' && c <= 'f'      ? c - 'a' + 10
        : c >= 'A' && c <= 'F'      ? c - 'A' + 10
                                    : -1;
    }
    private static long number(String value) throws Exception {
        if (!value.matches("0|[1-9][0-9]*"))
            throw new Exception();
        return Long.parseLong(value);
    }
    public static PairingLink parse(String uri) {
        try {
            String prefix = "lantern://pair?";
            if (uri == null || uri.length() > 2048 || !uri.startsWith(prefix) || uri.indexOf('#') >= 0)
                return null;
            Map<String, String> fields = new HashMap<>();
            for (String item : uri.substring(prefix.length()).split("&", -1)) {
                int eq = item.indexOf('=');
                if (eq < 0 || item.indexOf('=', eq + 1) >= 0)
                    return null;
                String key = item.substring(0, eq);
                if (!Arrays.asList("v", "n", "a", "p", "f", "c", "x").contains(key) ||
                    fields.containsKey(key))
                    return null;
                fields.put(key, decode(item.substring(eq + 1)));
            }
            if (fields.size() != 7 || !"1".equals(fields.get("v")))
                return null;
            String name = fields.get("n"), pin = fields.get("f").toLowerCase(Locale.ROOT),
                   code = fields.get("c");
            long port = number(fields.get("p")), expires = number(fields.get("x"));
            if (!validLabel(name) || !pin.matches("[0-9a-f]{64}") || !code.matches("[0-9a-f]{64}") ||
                port < 1 || port > 65535 || expires <= System.currentTimeMillis() / 1000 ||
                expires > 4102444800L)
                return null;
            List<String> addresses = new ArrayList<>();
            for (String a : fields.get("a").split(",", -1)) {
                if (!validAddress(a) || addresses.size() >= 8 || addresses.contains(a))
                    return null;
                addresses.add(a);
            }
            return new PairingLink(name, addresses, (int)port, pin, code, expires);
        } catch (Exception ignored) {
            return null;
        }
    }
}
