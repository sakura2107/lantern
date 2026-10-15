package org.lantern.client;

import android.app.*;
import android.content.*;
import android.os.*;
import android.util.AtomicFile;
import android.widget.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.json.*;

/** Durable metadata only; no credential is persisted here. One bounded disk/network worker. */
final class TransferQueue implements AutoCloseable {
    static final class Task {
        String id, host, pin, path, name;String sourceHash="";
        volatile String state = "等待", error = "", local = "";
        int port, kind;
        int retries,errorCode;long retryAt;
        boolean directory;
        volatile long current, total;
        JSONObject json() throws Exception {
            return new JSONObject()
                .put("id", id)
                .put("host", host)
                .put("pin", pin)
                .put("port", port)
                .put("path", path)
                .put("name", name)
                .put("state", state)
                .put("kind", kind)
                .put("directory", directory)
                .put("error", error)
                .put("local", local).put("sourceHash",sourceHash)
                .put("current", current)
                .put("total", total).put("retries",retries).put("errorCode",errorCode);
        }
        Task() {}
        Task(JSONObject j) throws Exception {
            id = j.getString("id");
            host = j.getString("host");
            pin = j.getString("pin");
            port = j.getInt("port");
            path = j.getString("path");
            name = j.getString("name");
            kind = j.getInt("kind");
            directory = j.getBoolean("directory");
            local = j.getString("local");sourceHash=j.optString("sourceHash","");
            current = j.getLong("current");
            total = j.getLong("total");
            state = j.getString("state");
            error = j.optString("error");retries=j.optInt("retries",0);errorCode=j.optInt("errorCode",0);
            if (!id.matches("[0-9a-f-]{36}") || kind < 0 || kind > 3)
                throw new IOException("任务记录无效");
            if (!state.equals("完成") && !state.equals("已取消"))
                state = "已暂停";
        }
    }
    final Context context;
    java.lang.ref.WeakReference<MainActivity> activity=new java.lang.ref.WeakReference<>(null);
    volatile Remote.Credentials credentials;
    final AtomicFile journal;
    final File root;
    final List<Task> tasks = new ArrayList<>();
    final AtomicBoolean cancel = new AtomicBoolean();
    final ExecutorService worker = Executors.newSingleThreadExecutor();
    final ScheduledExecutorService probes = Executors.newSingleThreadScheduledExecutor();
    volatile Task running;
    volatile Remote remote;
    volatile Remote.UploadSource activeSource;
    volatile java.net.Socket opening;
    volatile boolean closed;
    String storageError = "";
    TransferQueue(MainActivity activity){this(activity.getApplicationContext());attach(activity);credentials=activity.credentials;}
    TransferQueue(Context app){this(app,new File(app.getFilesDir(),"transfers"));}
    TransferQueue(Context app,File area) {
        context = app.getApplicationContext();
        root = area;
        root.mkdirs();
        journal = new AtomicFile(new File(root, "tasks.json"));
        try {
            if (journal.getBaseFile().exists()) {
                if (journal.getBaseFile().length() > 1024 * 1024)
                    throw new IOException("任务记录过大");
                JSONArray data =
                    new JSONArray(new String(journal.readFully(), java.nio.charset.StandardCharsets.UTF_8));
                if (data.length() > 32)
                    throw new IOException("任务数量越界");
                for (int i = 0; i < data.length(); i++)
                    tasks.add(new Task(data.getJSONObject(i)));
            }
        } catch (Exception e) {
            storageError = "任务记录无法恢复，请保留数据并检查：" + e.getMessage();
            status(storageError);
        }
        probes.scheduleWithFixedDelay(() -> {
            pump();
            Task task = running;
            Remote.Credentials c = credentials;
            if (task == null || !matches(task, c))
                return;
            try (Remote r = new Remote(c, 0, 0)) {
                long start = System.nanoTime();
                long nonce = start;
                DataInputStream reply = Remote.reader(r.call(50, Remote.body(o -> o.writeLong(nonce))));
                if (reply.readLong() != nonce || reply.available() != 0)
                    throw new IOException("网络探测响应错误");
                TransferPacer.SHARED.sample((System.nanoTime() - start) / 1e6);
            } catch (Exception ignored) {
            }
        }, 1, 1, TimeUnit.SECONDS);
    }
    void attach(MainActivity ui){activity=new java.lang.ref.WeakReference<>(ui);}
    void detach(MainActivity ui){if(activity.get()==ui)activity.clear();}
    void status(String text){MainActivity ui=activity.get();if(ui!=null&&!ui.isDestroyed())ui.status(text);}
    synchronized void setCredentials(Remote.Credentials c){if(running!=null&&!matches(running,c))pause(running,false);credentials=c;pump();}
    synchronized boolean hasPending(){for(Task task:tasks)if(matches(task,credentials)&&(task==running||task.state.equals("等待")||task.state.equals("等待网络")))return true;return false;}
    synchronized String notificationText(){if(running!=null)return Ui.size(running.current)+" / "+Ui.size(running.total);for(Task task:tasks)if(task.state.equals("等待网络")&&matches(task,credentials))return "连接中断，保留进度后自动重试（"+task.retries+"/"+RetryPolicy.MAX_ATTEMPTS+"）";return "等待任务";}
    void abortSocket(){try{if(opening!=null)opening.close();}catch(Exception ignored){}try{if(remote!=null)remote.abort();}catch(Exception ignored){}}
    synchronized void networkChanged(){if(running!=null)abortSocket();for(Task t:tasks)if(t.state.equals("等待网络"))t.retryAt=SystemClock.elapsedRealtime();pump();}
    synchronized void pauseAll(String reason){for(Task task:tasks)if(task==running||task.state.equals("等待")||task.state.equals("等待网络")){pause(task,false);task.error=reason;}try{persist();}catch(Exception e){status(e.getMessage());}}
    static boolean matches(Task t, Remote.Credentials c) {
        return c != null && t.host.equals(c.host) && t.pin.equals(c.pin) && t.port == c.port;
    }
    synchronized void persist() throws Exception {
        JSONArray array = new JSONArray();
        for (Task t : tasks)
            array.put(t.json());
        FileOutputStream out = null;
        try {
            out = journal.startWrite();
            out.write(array.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            journal.finishWrite(out);
        } catch (Exception e) {
            if (out != null)
                journal.failWrite(out);
            throw e;
        }
    }
    synchronized void add(String path, String name, boolean directory, int kind, String local) {
        if (credentials == null) {
            status("请先配对");
            return;
        }
        if (!storageError.isEmpty()) {
            status(storageError);
            return;
        }
        if (tasks.size() >= 32) {
            status("最多保留 32 项任务，请清理已完成项");
            return;
        }
        if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.equals(".") ||
            name.equals("..")) {
            status("文件名无效");
            return;
        }
        Task t = new Task();
        t.id = UUID.randomUUID().toString();
        t.host = credentials.host;
        t.pin = credentials.pin;
        t.port = credentials.port;
        t.path = path;
        t.name = name;
        t.directory = directory;
        t.kind = kind;
        t.local = local;
        tasks.add(t);
        try {
            persist();
            pump();
            status("已加入传输任务：" + name);
        } catch (Exception e) {
            tasks.remove(t);
            status("保存任务失败：" + e.getMessage());
        }
    }
    synchronized void pump() {
        if (closed || running != null)
            return;
        for (Task t : tasks)
            if ((t.state.equals("等待")||(t.state.equals("等待网络")&&SystemClock.elapsedRealtime()>=t.retryAt)) && matches(t, credentials)) {
                try{TransferService.ensure(context,this);}catch(RuntimeException error){t.state="已暂停";t.error="系统拒绝后台启动，请回到应用继续";try{persist();}catch(Exception ignored){}status(t.error);return;}
                running = t;
                cancel.set(false);
                Remote.Credentials c = credentials;
                worker.execute(() -> run(t, c));
                return;
            }
    }
    void progress(Task t, long n, long total) {
        t.current = n;
        t.total = total;
        status((t.kind == 0 ? "下载 " : "上传 ") + t.name + "：" + n + " / " + total + "；任务列表可暂停");
    }
    void ensureSpace(long needed) throws IOException {
        long used;
        try (java.util.stream.Stream<java.nio.file.Path> files = java.nio.file.Files.walk(root.toPath())) {
            used = files.filter(java.nio.file.Files::isRegularFile).mapToLong(p -> p.toFile().length()).sum();
        }
        if (used + needed > 16L * 1024 * 1024 * 1024)
            throw new IOException("传输缓存超过 16 GiB，请清理已下载文件");
        if (needed > 16L * 1024 * 1024 * 1024 ||
            new StatFs(root.getPath()).getAvailableBytes() < needed + 64L * 1024 * 1024)
            throw new IOException("暂存空间不足或单任务超过 16 GiB");
    }
    void run(Task t, Remote.Credentials c) {
        try {
            synchronized (this) {
                t.state = "进行中";
                persist();
            }
            try (Remote r = new Remote(c, t.kind == 0 ? 1 : 3, 3, s -> {
                     opening = s;
                     if (cancel.get())
                         s.close();
                 })) {
                remote = r;
                if(t.kind==3){
                    try(SafSource source=SafSource.open(context.getContentResolver(),android.net.Uri.parse(t.local))){if(source==null)throw new IOException("源应用不再支持可寻址读取，请重新选择文件以暂存上传");activeSource=source;String hash=source.hash(cancel);if(!t.sourceHash.isEmpty()&&!t.sourceHash.equals(hash))throw new IOException("源文件已变化，请保留远端暂存并创建新任务");synchronized(this){t.sourceHash=hash;persist();}r.upload(source,t.path,cancel,(n,total)->progress(t,n,total));}finally{activeSource=null;}
                }else if (t.kind == 2)
                    TreeUpload.run(this, t, r);
                else if (t.kind == 1) {
                    File source = new File(t.local);
                    r.upload(source, t.path, cancel, (n, total) -> progress(t, n, total));
                } else if (t.directory)
                    downloadDirectory(t, r);
                else {
                    File dir = new File(root, t.id);
                    dir.mkdirs();
                    File target = new File(dir, t.name);
                    Remote.Info info = r.stat(t.path, false);
                    ensureSpace(Math.max(0, info.size - new File(target + ".part").length()));
                    r.download(t.path, target, cancel, (n, total) -> progress(t, n, total));
                    t.local = target.getAbsolutePath();
                }
            }
            synchronized (this) {
                t.state = "完成";
                t.error = "";t.retries=0;t.errorCode=0;
                persist();
            }
        } catch (Exception error) {
            synchronized (this) {
                t.errorCode=error instanceof Remote.RemoteError?((Remote.RemoteError)error).code:0;
                if(!t.state.equals("已取消")){
                    if(cancel.get())t.state="已暂停";
                    else if(!closed&&RetryPolicy.transientFailure(error)&&t.retries<RetryPolicy.MAX_ATTEMPTS){t.state="等待网络";t.retries++;t.retryAt=SystemClock.elapsedRealtime()+RetryPolicy.delayMillis(t.retries);}
                    else t.state=t.errorCode==4&&t.kind!=0?"等待处理":"失败";
                }
                t.error = error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage();
                try {
                    persist();
                } catch (Exception save) {
                    storageError = "任务状态保存失败：" + save.getMessage();
                }
                status(t.state + "：" + t.error);
            }
        } finally {
            remote = null;
            opening = null;
            synchronized (this) {
                running = null;
                pump();
            }
        }
    }
    static final class Node {
        String path, version;
        boolean directory;
        long size;
        Node(String p, boolean d, long s, String v) {
            path = p;
            directory = d;
            size = s;
            version = v;
        }
    }
    void downloadDirectory(Task t, Remote r) throws Exception {
        File base = new File(root, t.id);
        base.mkdirs();
        File manifest = new File(base, "manifest.json"), stage = new File(base, "partial"),
             dest = new File(base, t.name);
        List<Node> nodes = new ArrayList<>();
        if (dest.exists()) {
            t.local = dest.getAbsolutePath();
            return;
        }
        if (manifest.exists()) {
            if (manifest.length() > 8 * 1024 * 1024)
                throw new IOException("目录清单超限");
            JSONArray data = new JSONArray(new String(java.nio.file.Files.readAllBytes(manifest.toPath()),
                                                      java.nio.charset.StandardCharsets.UTF_8));
            if (data.length() > 20000)
                throw new IOException("目录项目数超限");
            for (int i = 0; i < data.length(); i++) {
                JSONObject j = data.getJSONObject(i);
                nodes.add(new Node(j.getString("path"), j.getBoolean("dir"), j.getLong("size"),
                                   j.getString("version")));
            }
        } else {
            ArrayDeque<String> pending = new ArrayDeque<>();
            pending.add("");
            long names = 0;
            while (!pending.isEmpty()) {
                if (cancel.get())
                    throw new IOException("已暂停目录扫描");
                String rel = pending.remove();
                if (rel.split("/", -1).length > 32)
                    throw new IOException("目录深度超限");
                int offset = 0;
                String generation = "";
                do {
                    Remote.Page page =
                        r.page(rel.isEmpty() ? t.path : t.path + "/" + rel, offset, "", 0, false, generation);
                    generation = page.generation;
                    for (Remote.Entry e : page.entries) {
                        String sub = rel.isEmpty() ? e.name : rel + "/" + e.name;
                        if (nodes.size() >= 20000 || (names += sub.length() * 2L) > 4 * 1024 * 1024)
                            throw new IOException("目录超过 20000 项或名称预算");
                        Remote.Info info = e.directory ? null : r.stat(t.path + "/" + sub, false);
                        nodes.add(new Node(sub, e.directory, e.size, info == null ? "" : info.version));
                        if (e.directory)
                            pending.add(sub);
                    }
                    offset += page.entries.size();
                    if (offset >= page.total)
                        break;
                    if (page.entries.isEmpty())
                        throw new IOException("目录分页无进展");
                } while (true);
            }
            JSONArray data = new JSONArray();
            for (Node node : nodes)
                data.put(new JSONObject()
                             .put("path", node.path)
                             .put("dir", node.directory)
                             .put("size", node.size)
                             .put("version", node.version));
            AtomicFile file = new AtomicFile(manifest);
            FileOutputStream out = file.startWrite();
            try {
                out.write(data.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                file.finishWrite(out);
            } catch (Exception e) {
                file.failWrite(out);
                throw e;
            }
        }
        stage.mkdirs();
        long total = 0;
        for (Node n : nodes)
            if (!n.directory)
                total = Math.addExact(total, n.size);
        long remaining=total;
        for(Node node:nodes)if(!node.directory){File target=new File(stage,node.path);File partial=new File(target+".part");remaining-=Math.min(node.size,target.exists()?target.length():partial.length());}
        ensureSpace(Math.max(0,remaining));
        long completed = 0;
        for (Node n : nodes) {
            if (cancel.get())
                throw new IOException("目录传输已暂停");
            File local = new File(stage, n.path);
            if (!local.getCanonicalPath().startsWith(stage.getCanonicalPath() + File.separator))
                throw new IOException("目录路径越界");
            if (n.directory) {
                if (!local.mkdirs() && !local.isDirectory())
                    throw new IOException("创建本地目录失败");
                continue;
            }
            File parent = local.getParentFile();
            parent.mkdirs();
            Remote.Info current = r.stat(t.path + "/" + n.path, false);
            if (!current.version.equals(n.version) || current.size != n.size)
                throw new IOException("目录源文件已变化：" + n.path + "；保留原暂存");
            long before = completed, all = total;
            r.download(t.path + "/" + n.path, local, cancel, (done, size) -> progress(t, before + done, all));
            completed += n.size;
        }
        if (cancel.get())
            throw new IOException("目录传输已暂停");
        java.nio.file.Files.move(stage.toPath(), dest.toPath());
        t.local = dest.getAbsolutePath();
    }
    synchronized void pause(Task t, boolean discard) {
        t.state = discard ? "已取消" : "已暂停";
        if (running == t) {
            cancel.set(true);
            Remote.UploadSource source=activeSource;if(source!=null)try{source.close();}catch(Exception ignored){}
            try {
                if (opening != null)
                    opening.close();
            } catch (Exception ignored) {
            }
            try {
                if (remote != null)
                    remote.abort();
            } catch (Exception ignored) {
            }
        }
        try {
            persist();
        } catch (Exception e) {
            status(e.getMessage());
        }
    }
    synchronized void resume(Task t) {
        if (running == t)
            return;
        t.state = "等待";
        t.error = "";t.retries=0;t.errorCode=0;t.retryAt=0;
        try {
            persist();
            pump();
        } catch (Exception e) {
            status(e.getMessage());
        }
    }
    /** Deletes the task's private cache directory and, for staged uploads, the staged copy. */
    void deleteTaskFiles(Task task) throws IOException {
        File directory = new File(root, task.id);
        if (!directory.getCanonicalFile().getParentFile().equals(root.getCanonicalFile()))
            throw new IOException("缓存路径无效");
        if (directory.exists())
            try (java.util.stream.Stream<java.nio.file.Path> paths =
                     java.nio.file.Files.walk(directory.toPath())) {
                for (java.nio.file.Path path :
                     (Iterable<java.nio.file.Path>)paths.sorted(Comparator.reverseOrder())::iterator)
                    java.nio.file.Files.deleteIfExists(path);
            }
        if (task.kind == 1) {
            File source = new File(task.local);
            if (source.getCanonicalFile().getParentFile().equals(root.getCanonicalFile()) &&
                source.getName().startsWith("upload-"))
                source.delete();
        }
    }
    synchronized void clearCompleted() {
        try {
            for (Iterator<Task> it = tasks.iterator(); it.hasNext();) {
                Task task = it.next();
                if (!task.state.equals("完成"))
                    continue;
                deleteTaskFiles(task);
                it.remove();
            }
            persist();
            status("已清理完成任务及应用下载缓存");
        } catch (Exception e) {
            status(e.getMessage());
        }
    }
    /** Removes one finished or cancelled task and its private cache; host files are untouched. */
    synchronized void remove(Task task) {
        try {
            if (running == task || !(task.state.equals("完成") || task.state.equals("已取消")))
                throw new IOException("只能移除已完成或已取消的任务");
            deleteTaskFiles(task);
            tasks.remove(task);
            persist();
            status("已移除任务：" + task.name);
        } catch (Exception e) {
            status(e.getMessage());
        }
    }
    synchronized void moveToTop(Task task) {
        if (!tasks.remove(task))
            return;
        tasks.add(0, task);
        try {
            persist();
        } catch (Exception e) {
            status(e.getMessage());
        }
    }
    void openDone(Task task) {
        MainActivity ui = activity.get();
        if (ui == null || ui.isDestroyed())
            return;
        if (task.kind != 0 || task.directory || !task.state.equals("完成")) {
            status("只有已完成的单个下载文件可以直接打开");
            return;
        }
        try {
            ui.open(FilesProvider.share(new FilesProvider.Source(new File(task.local))), task.name);
        } catch (Exception e) {
            status(e.getMessage());
        }
    }
    synchronized void renameConflict(Task task,String name)throws Exception{
        TreeUpload.safeName(name);if(task.kind==0||running==task||!task.state.equals("等待处理"))throw new IOException("此任务无法更改目标");
        int slash=task.path.lastIndexOf('/');String path=(slash<0?"":task.path.substring(0,slash+1))+name;
        if(task.kind==2){pause(task,true);add(path,name,true,2,task.local);return;}
        task.path=path;task.name=name;resume(task);
    }
    public synchronized void close() {
        closed = true;
        if (running != null)
            pause(running, false);
        worker.shutdown();
        probes.shutdownNow();
    }
}
