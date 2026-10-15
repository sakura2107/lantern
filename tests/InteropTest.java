package org.lantern.client;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;
public final class InteropTest {
    static int assertions = 0;
    static void check(boolean result, String name) throws Exception {
        assertions++;
        if (!result)
            throw new IOException(name);
    }
    public static void main(String[] args) throws Exception {
        BufferedReader stdin =
            new BufferedReader(new InputStreamReader(System.in, java.nio.charset.StandardCharsets.UTF_8));
        String pin = stdin.readLine(), token = stdin.readLine(), directory = stdin.readLine();
        String pairingUri = stdin.readLine();
        PairingLink pairingLink = PairingLink.parse(pairingUri);
        check(pairingLink != null, "Java pairing URI parsed");
        var paired = Remote.pair(pairingLink, "Java phone fixture");
        check(paired.host.equals("127.0.0.1") && paired.token.matches("[0-9a-f]{64}"),
              "Java pinned TLS pairing with address failover");
        try (Remote remote = new Remote(paired, 1, 3)) {
            check(!remote.list("", 0).isEmpty(), "Java QR credential accepted by HELLO");
        }
        boolean reused = false;
        try {
            Remote.pair(pairingLink, "Replay");
        } catch (Remote.PairingException error) {
            reused = error.reason == Remote.PairFailure.INVALID_INVITE;
        }
        check(reused, "Java typed invitation replay error");
        String wrongPin = pairingUri.replace("f=" + pairingLink.pin, "f="
                                                                         + "0".repeat(64));
        boolean mismatch = false;
        try {
            Remote.pair(PairingLink.parse(wrongPin), "Pin");
        } catch (Remote.PairingException error) {
            mismatch = error.reason == Remote.PairFailure.PIN_MISMATCH;
        }
        check(mismatch, "Java pin mismatch distinguished without sending invitation");
        var credentials = new Remote.Credentials("127.0.0.1", 47449, pin, token);
        boolean pinDenied = false;
        try (Remote ignored =
                 new Remote(new Remote.Credentials("127.0.0.1", 47449, "0".repeat(64), token), 1, 3)) {
        } catch (Exception e) {
            pinDenied = true;
        }
        check(pinDenied, "Java rejects wrong pin");
        try(Remote identity=new Remote(credentials,0,0)) {
            check(Remote.str(Remote.reader(identity.call(30,new byte[0]))).startsWith("Lantern/"),"identity pairing independent of file scope");
            boolean denied=false;
            try {identity.list("",0);} catch(Remote.RemoteError error) {denied=error.code==2;}
            check(denied,"typed permission error preserved through shared JNI parser");
        }
        try (Remote remote = new Remote(credentials, 3, 3)) {
            check(!remote.list("", 0).isEmpty(), "Java TLS list");
            var info = remote.stat("source.bin", true);
            check(info.size == 4195035, "metadata matches");
            byte[] random = remote.read("source.bin", info.version, 513, 16384);
            check(random.length == 16384 && random[0] == (byte)((513 * 97 + 513 / 731) % 256),
                  "original media read range");
            File target = Path.of(directory, "java-download.bin").toFile();
            AtomicBoolean cancel = new AtomicBoolean();
            boolean paused = false;
            try {
                remote.download("source.bin", target, cancel, (n, t) -> {
                    if (n >= Remote.CHUNK)
                        cancel.set(true);
                });
            } catch (IOException e) {
                paused = true;
            }
            check(paused && !target.exists(), "Java download cancel keeps partial");
            cancel.set(false);
            long[] resumed = {0};
            remote.download("source.bin", target, cancel, (n, t) -> {
                if (resumed[0] == 0)
                    resumed[0] = n;
            });
            check(resumed[0] == Remote.CHUNK, "Java download resumed");
            check(Remote.hash(target).equals(info.hash), "Java download verified");
            cancel.set(false);
            boolean uploadPaused = false;
            try {
                remote.upload(target, "java-upload.bin", cancel, (n, t) -> {
                    if (n >= Remote.CHUNK)
                        cancel.set(true);
                });
            } catch (IOException e) {
                uploadPaused = true;
            }
            check(uploadPaused, "Java upload cancelled");
            cancel.set(false);
            resumed[0] = 0;
            remote.upload(target, "java-upload.bin", cancel, (n, t) -> {
                if (resumed[0] == 0)
                    resumed[0] = n;
            });
            check(resumed[0] == Remote.CHUNK, "Java upload resumed");
            check(remote.stat("java-upload.bin", true).hash.equals(info.hash), "Java upload verified");
            boolean malformed = false;
            try {
                Native.response(1, new byte[] {0, 1, 0, 1, 0});
            } catch (IOException e) {
                malformed = true;
            }
            check(malformed, "JNI truncated response rejects");
        }
        System.out.println(assertions +
                           " Java/JNI interoperability assertions passed against actual C++ TLS host");
    }
}
