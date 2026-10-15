import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/**
 * Rebuilds a debug APK without Gradle: keeps the base APK's native libraries and assets, swaps in a fresh
 * classes.dex and takes the manifest, resources.arsc and res/ (launcher icons) from an aapt2-linked APK.
 * Usage: java Repack.java base.apk classes.dex resources.apk out.apk  (then zipalign -P 16 4 and apksigner).
 * Old signatures are dropped. Each entry keeps its source compression, so .so files and resources.arsc stay stored.
 */
public class Repack {
    public static void main(String[] a) throws Exception {
        try (ZipFile base = new ZipFile(a[0]); ZipFile res = new ZipFile(a[2]);
             ZipOutputStream out = new ZipOutputStream(new FileOutputStream(a[3]))) {
            Map<String, ZipEntry> fresh = new LinkedHashMap<>();
            for (ZipEntry e : Collections.list(res.entries()))
                if (resource(e.getName())) fresh.put(e.getName(), e);
            if (!fresh.containsKey("AndroidManifest.xml") || !fresh.containsKey("resources.arsc"))
                throw new IOException("resources APK lacks AndroidManifest.xml or resources.arsc");
            for (ZipEntry e : fresh.values())
                put(out, e.getName(), res.getInputStream(e).readAllBytes(), e.getMethod());
            int kept = 0;
            for (ZipEntry e : Collections.list(base.entries())) {
                String n = e.getName();
                if (n.startsWith("META-INF/") || (n.startsWith("classes") && n.endsWith(".dex")) || resource(n)) continue;
                put(out, n, base.getInputStream(e).readAllBytes(), e.getMethod());
                kept++;
            }
            put(out, "classes.dex", Files.readAllBytes(Paths.get(a[1])), ZipEntry.DEFLATED);
            System.out.println("repacked: " + fresh.size() + " resource entries, " + kept + " base entries, classes.dex");
        }
    }

    static boolean resource(String n) {
        return n.equals("AndroidManifest.xml") || n.equals("resources.arsc") || n.startsWith("res/");
    }

    static void put(ZipOutputStream out, String n, byte[] data, int method) throws IOException {
        ZipEntry e = new ZipEntry(n);
        e.setMethod(method);
        if (method == ZipEntry.STORED) {
            CRC32 c = new CRC32(); c.update(data);
            e.setSize(data.length); e.setCompressedSize(data.length); e.setCrc(c.getValue());
        }
        out.putNextEntry(e); out.write(data); out.closeEntry();
    }
}
