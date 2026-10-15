package org.lantern.client;
import java.io.IOException;
final class Native {
    static {
        System.loadLibrary("lantern_jni");
    }
    static native byte[] packet(int operation, byte[] payload) throws IOException;
    static native byte[] response(int operation, byte[] packet) throws IOException;
}
