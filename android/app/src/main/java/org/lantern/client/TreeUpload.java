package org.lantern.client;
import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.AtomicFile;
import java.io.*;
import java.util.*;
import org.json.*;

/** Upload only the user-selected SAF tree. Manifest is durable; each final file commits separately. */
final class TreeUpload {
    static final String[] COLUMNS = {"document_id", "_display_name", "mime_type", "_size", "last_modified"};
    static void safeName(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") ||
            name.contains("\\"))
            throw new IOException("目录中文件名无效");
    }
    static void save(AtomicFile file, JSONArray nodes) throws Exception {
        FileOutputStream out = file.startWrite();
        try {
            byte[] bytes = nodes.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
            if (bytes.length > 8 * 1024 * 1024)
                throw new IOException("上传清单超过 8 MiB");
            out.write(bytes);
            file.finishWrite(out);
        } catch (Exception e) {
            file.failWrite(out);
            throw e;
        }
    }
    static long[] version(ContentResolver resolver, Uri uri) throws IOException {
        try (Cursor cursor = resolver.query(uri, new String[] {"_size", "last_modified"}, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst())
                throw new IOException("源应用无法提供文件信息");
            return new long[] {cursor.isNull(0) ? -1 : cursor.getLong(0),
                               cursor.isNull(1) ? 0 : cursor.getLong(1)};
        }
    }
    static void directory(Remote remote, String path) throws Exception {
        try {
            remote.page(path, 0, "", 0, false, "");
        } catch (Remote.RemoteError e) {
            if (e.code != 3)
                throw e;
            remote.mutate(8, path, "");
        }
    }
    static void run(TransferQueue owner, TransferQueue.Task task, Remote remote) throws Exception {
        ContentResolver resolver = owner.context.getContentResolver();
        Uri tree = Uri.parse(task.local);
        File base = new File(owner.root, task.id);
        base.mkdirs();
        AtomicFile manifest = new AtomicFile(new File(base, "upload-tree.json"));
        JSONArray nodes;
        if (manifest.getBaseFile().exists()) {
            if (manifest.getBaseFile().length() > 8 * 1024 * 1024)
                throw new IOException("上传清单过大");
            nodes = new JSONArray(new String(manifest.readFully(), java.nio.charset.StandardCharsets.UTF_8));
            if (nodes.length() > 20000)
                throw new IOException("上传清单项目数超限");
        } else {
            nodes = new JSONArray();
            ArrayDeque<String[]> pending = new ArrayDeque<>();
            pending.add(new String[] {DocumentsContract.getTreeDocumentId(tree), ""});
            int names = 0;
            while (!pending.isEmpty()) {
                if (owner.cancel.get())
                    throw new IOException("目录扫描已暂停");
                String[] at = pending.remove();
                if (at[1].split("/", -1).length > 32)
                    throw new IOException("上传目录深度超过 32");
                Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, at[0]);
                try (Cursor cursor = resolver.query(children, COLUMNS, null, null, null)) {
                    if (cursor == null)
                        throw new IOException("源应用不支持目录枚举");
                    while (cursor.moveToNext()) {
                        String id = cursor.getString(0), name = cursor.getString(1);
                        safeName(name);
                        String path = at[1].isEmpty() ? name : at[1] + "/" + name;
                        if (nodes.length() >= 20000 || (names += path.length()) > 1024 * 1024)
                            throw new IOException("目录项目数或名称预算超限");
                        boolean dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2));
                        nodes.put(
                            new JSONObject()
                                .put("path", path)
                                .put("uri", DocumentsContract.buildDocumentUriUsingTree(tree, id).toString())
                                .put("dir", dir)
                                .put("size", cursor.isNull(3) ? -1 : cursor.getLong(3))
                                .put("modified", cursor.isNull(4) ? 0 : cursor.getLong(4))
                                .put("done", false));
                        if (dir)
                            pending.add(new String[] {id, path});
                    }
                }
            }
            save(manifest, nodes);
        }
        directory(remote, task.path);
        long completed = 0;
        task.total = nodes.length();
        for (int i = 0; i < nodes.length(); i++) {
            if (owner.cancel.get())
                throw new IOException("目录上传已暂停，已提交文件保留");
            JSONObject node = nodes.getJSONObject(i);
            String destination = task.path + "/" + node.getString("path");
            if (node.getBoolean("dir")) {
                directory(remote, destination);
                completed++;
                owner.progress(task, completed, nodes.length());
                continue;
            }
            Uri uri = Uri.parse(node.getString("uri"));
            long[] current = version(resolver, uri);
            if (current[0] != node.getLong("size") || current[1] != node.getLong("modified"))
                throw new IOException("本地源文件已变化：" + node.getString("path"));
            if (node.getBoolean("done")) {
                Remote.Info existing = remote.hashSnapshot(destination, owner.cancel, (n, t) -> {});
                if (!existing.hash.equals(node.getString("hash")))
                    throw new IOException("已提交的远端文件发生变化：" + destination);
                completed++;
                continue;
            }
            try(SafSource source=SafSource.open(resolver,uri)){
                if(source!=null){owner.activeSource=source;try{String hash=source.hash(owner.cancel);if(node.has("hash")&&!node.getString("hash").equals(hash))throw new IOException("源文件在续传前变化："+node.getString("path"));node.put("hash",hash);save(manifest,nodes);remote.upload(source,destination,owner.cancel,(n,total)->owner.status("目录直传："+node.optString("path")+" "+n+" / "+total));node.put("done",true);save(manifest,nodes);completed++;owner.progress(task,completed,nodes.length());continue;}finally{owner.activeSource=null;}}
            }
            File snapshot = new File(base, "source.snapshot");
            long snapshotLimit=current[0]<0?1024L*1024*1024:current[0];
            owner.ensureSpace(snapshotLimit);
            try {
                try (InputStream in = resolver.openInputStream(uri);
                     FileOutputStream out = new FileOutputStream(snapshot)) {
                    if (in == null)
                        throw new IOException("源文件无法读取");
                    byte[] buffer = new byte[Remote.CHUNK];
                    int count;
                    long bytes = 0;
                    while ((count = in.read(buffer)) != -1) {
                        if (owner.cancel.get())
                            throw new IOException("上传暂存已暂停");
                        bytes += count;
                        if (bytes > Math.min(snapshotLimit,16L * 1024 * 1024 * 1024))
                            throw new IOException("单文件超过 16 GiB");
                        if(new android.os.StatFs(base.getPath()).getAvailableBytes()<count+64L*1024*1024)throw new IOException("本地暂存空间不足");
                        out.write(buffer, 0, count);
                    }
                    out.getFD().sync();
                    if (current[0] >= 0 && bytes != current[0])
                        throw new IOException("源文件长度变化");
                }
                if (!Arrays.equals(current, version(resolver, uri)))
                    throw new IOException("暂存期间源文件变化");
                String hash = Remote.hash(snapshot, owner.cancel);
                boolean exists = false;
                try {
                    Remote.Info info = remote.hashSnapshot(destination, owner.cancel, (n, t) -> {});
                    if (!info.hash.equals(hash))
                        throw new Remote.RemoteError(4,"同名文件内容不同，保留远端原件：" + destination);
                    exists = true;
                } catch (Remote.RemoteError e) {
                    if (e.code != 3)
                        throw e;
                }
                if (!exists)
                    remote.upload(snapshot, destination, owner.cancel,
                                  (n, total)
                                      -> owner.status("目录上传：" + node.optString("path") + " " + n +
                                                         " / " + total));
                node.put("hash", hash).put("done", true);
                save(manifest, nodes);
                completed++;
                owner.progress(task, completed, nodes.length());
            } finally {
                snapshot.delete();
            }
        }
    }
}
