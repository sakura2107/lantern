package org.lantern.client;
public final class PairingLinkTest {
    static int checks;
    static void check(boolean ok) {
        if (!ok)
            throw new AssertionError("PairingLink assertion");
        checks++;
    }
    public static void main(String[] args) {
        String pin = "a".repeat(64), code = "b".repeat(64);
        String uri =
            "lantern://pair?v=1&n=%E7%94%B5%E8%84%91%20A%26B&a=192.168.1.5,10.0.0.2&p=47443&f=" + pin +
            "&c=" + code + "&x=" + (System.currentTimeMillis() / 1000 + 600);
        PairingLink link = PairingLink.parse(uri);
        check(link != null && link.hostName.equals("电脑 A&B") && link.addresses.size() == 2 &&
              link.port == 47443);
        for (String value :
             new String[] {uri + "&v=1", uri + "&unknown=x", uri + "&", uri + "#x", "a".repeat(2049)})
            check(PairingLink.parse(value) == null);
        check(PairingLink.parse(null) == null);
        for (String value : new String[] {"0", "2", "01"})
            check(PairingLink.parse(uri.replace("v=1", "v=" + value)) == null);
        for (String value : new String[] {"0", "65536", "-1", "047443", "12x"})
            check(PairingLink.parse(uri.replace("p=47443", "p=" + value)) == null);
        for (String value : new String[] {"example.com", "127.1", "192.168.001.5", "256.1.1.1", "0.0.0.0",
                                          "224.0.0.1", "192.168.1.5,192.168.1.5", "192.168.1.5,"})
            check(PairingLink.parse(uri.replace("a=192.168.1.5,10.0.0.2", "a=" + value)) == null);
        for (String value :
             new String[] {"%", "%GG", "%00", "%C0%AF", "%ED%A0%80", "%F4%90%80%80", "%0A", ""})
            check(PairingLink.parse(uri.replace("n=%E7%94%B5%E8%84%91%20A%26B", "n=" + value)) == null);
        check(PairingLink.parse(uri.substring(0, uri.indexOf("&x=")) + "&x=1") == null);
        check(PairingLink.parse(uri.replace("f=" + pin, "f=abcd")) == null);
        check(PairingLink.parse(uri.replace("c=" + code, "c=wrong")) == null);
        System.out.println(checks + " Java pairing-link checks passed");
    }
}
