package org.lantern.client;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.widget.*;
import java.io.IOException;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Owns file requests; desktop subscriptions are deliberately independent. */
final class FileBrowser implements AutoCloseable {
    final MainActivity ui;
    final ThreadPoolExecutor executor =
        new ThreadPoolExecutor(1, 1, 0, TimeUnit.SECONDS, new ArrayBlockingQueue<>(2));
    final AtomicLong epoch = new AtomicLong();
    volatile Remote active;
    volatile java.net.Socket opening;
    volatile String directory = "";
    String generation = "", generationPath = "";
    int sort;
    boolean descending, selecting;
    final LinkedHashSet<String> selected = new LinkedHashSet<>();
    FileBrowser(MainActivity activity) {
        ui = activity;
    }
    String resolve(String name) {
        return directory.isEmpty() ? name : directory + "/" + name;
    }
    FilesPage page() { return ui.ui.files; }
    void cancel() {
        epoch.incrementAndGet();
        executor.getQueue().clear();
        try {
            if (opening != null)
                opening.close();
        } catch (Exception ignored) {
        }
        try {
            if (active != null)
                active.abort();
        } catch (Exception ignored) {
        }
        ui.runOnUiThread(() -> page().loading(false, null));
    }
    interface Request {
        void run(Remote remote, long ticket) throws Exception;
    }
    /** label == null: a quiet listing load; otherwise a host operation shown with a cancel action. */
    void request(int scopes, String label, Request job) {
        cancel();
        long ticket = epoch.get();
        Remote.Credentials c = ui.credentials;
        if (c == null) {
            ui.status(S.NEED_DEVICE);
            return;
        }
        page().loading(true, label);
        try {
            executor.execute(() -> {
                try (Remote r = new Remote(c, scopes, 0, s -> {
                         opening = s;
                         if (epoch.get() != ticket)
                             s.close();
                     })) {
                    active = r;
                    if (ticket != epoch.get())
                        return;
                    job.run(r, ticket);
                } catch (Exception e) {
                    if (ticket == epoch.get()) {
                        ui.status(e.getMessage());
                        if (label == null) // a listing failed: show why instead of an "empty folder"
                            ui.runOnUiThread(() -> {
                                if (epoch.get() != ticket) return;
                                ui.entries = new ArrayList<>();
                                page().loadError = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                                ui.ui.renderFiles(ui.entries, selecting);
                                page().updateEmpty();
                            });
                    }
                } finally {
                    active = null;
                    opening = null;
                    ui.runOnUiThread(() -> { if (epoch.get() == ticket) page().loading(false, null); });
                }
            });
        } catch (RejectedExecutionException e) {
            page().loading(false, null);
            ui.status("文件操作正在结束，请稍后重试");
        }
    }
    void publish(List<Remote.Entry> items, long ticket, int total, int offset, String query) {
        ui.runOnUiThread(() -> {
            if (epoch.get() != ticket)
                return;
            ui.entries = items;
            selected.clear();
            page().noReadPermission = false;
            page().loadError = null;
            page().pageInfo(total, offset, items.size(), query);
            ui.ui.renderFiles(items, selecting);
        });
    }
    void refresh() {
        directory = ui.path.getText().toString();
        String path = directory;
        int offset = ui.offset;
        String expected = offset == 0 || !generationPath.equals(path) ? "" : generation;
        request(1, null, (r, t) -> {
            Remote.Page page = r.page(path, offset, "", sort, descending, expected);
            generation = page.generation;
            generationPath = path;
            publish(page.entries, t, page.total, offset, null);
        });
    }
    void search(String query) {
        directory = ui.path.getText().toString();
        String base = directory;
        request(1, String.format(Locale.ROOT, S.SEARCHING, 0, 0L), (r, t) -> {
            List<Remote.Entry> hits = new ArrayList<>();
            Remote.SearchPage page = r.search(0, base, query);
            for (;;) {
                if (epoch.get() != t)
                    return;
                hits.addAll(page.entries);
                if (hits.size() > 4096)
                    throw new IOException("结果超过 4096 项，请缩小搜索范围");
                final int found = hits.size();
                final long scanned = page.scanned;
                final boolean done = page.done;
                publish(new ArrayList<>(hits), t, found, 0, query);
                ui.runOnUiThread(() -> { if (epoch.get() == t) page().searchProgress(found, scanned, done); });
                if (page.done) {
                    ui.status(String.format(Locale.ROOT, S.SEARCH_DONE, found, scanned));
                    return;
                }
                page = r.search(1, "", "");
            }
        });
    }
    void newFolder() {
        Sheet.prompt(ui, S.NEW_FOLDER, null, S.NEW_FOLDER_HINT, "", S.CREATE, name -> {
            String target = resolve(name);
            request(2, S.WORKING_CREATE, (r, t) -> {
                r.mutate(8, target, "");
                ui.status(String.format(Locale.ROOT, S.CREATED, name));
                ui.runOnUiThread(this::refresh);
            });
        });
    }
    void rename(String source) {
        Sheet.prompt(ui, S.RENAME_MOVE, S.RENAME_HELP, S.HINT_PATH, source, S.SAVE, dest -> request(2, S.WORKING_RENAME, (r, t) -> {
            r.mutate(9, source, FilesPage.trimPath(dest));
            ui.runOnUiThread(this::refresh);
        }));
    }
    void copyOne(String source) {
        Sheet.prompt(ui, S.HOST_COPY, S.COPY_HELP, S.HINT_PATH, source + S.COPY_SUFFIX, S.COPY_CONFIRM,
                     dest -> singleCopy(source, FilesPage.trimPath(dest)));
    }
    void info(Remote.Entry item, String remote) {
        StringBuilder text = new StringBuilder();
        text.append(S.INFO_TYPE).append(item.directory ? S.FOLDER : S.FILE).append('\n');
        text.append(S.INFO_PATH).append(remote).append('\n');
        if (!item.directory)
            text.append(S.INFO_SIZE).append(Ui.size(item.size)).append(String.format(Locale.ROOT, S.INFO_BYTES, item.size)).append('\n');
        text.append(S.INFO_MODIFIED).append(Ui.fullDate(item.modified));
        Sheet.info(ui, Ui.baseName(item.name), text);
    }
    void thumbnail(Remote.Entry item, String source) {
        request(1, S.LOADING_PREVIEW, (r, t) -> {
            byte[] jpeg = r.thumbnail(source);
            Bitmap bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
            if (bitmap == null)
                throw new IOException("缩略图解码失败");
            ui.runOnUiThread(() -> {
                if (epoch.get() != t || ui.isDestroyed()) {
                    bitmap.recycle();
                    return;
                }
                ImageView image = new ImageView(ui);
                image.setImageBitmap(bitmap);
                image.setAdjustViewBounds(true);
                image.setScaleType(ImageView.ScaleType.FIT_CENTER);
                image.setBackground(Ui.round(Theme.INSET, 16));
                image.setClipToOutline(true);
                image.setMinimumHeight(Theme.dp(160));
                LinearLayout box = Ui.column(ui);
                box.setPadding(Theme.dp(16), Theme.dp(4), Theme.dp(16), Theme.dp(8));
                box.addView(image, Ui.lp(-1, -2));
                new Sheet(ui, Ui.baseName(item.name), S.THUMB_NOTE).view(box)
                    .buttons(null, S.OPEN_WITH_APP, Ui.SECONDARY, () -> ui.openRemote(item, source, 0))
                    .onDismiss(() -> { image.setImageDrawable(null); bitmap.recycle(); })
                    .show();
            });
        });
    }
    void confirmDelete(List<String> paths, Runnable before) {
        String title = paths.size() == 1 ? String.format(Locale.ROOT, S.DELETE_ONE, Ui.baseName(paths.get(0)))
                                         : String.format(Locale.ROOT, S.DELETE_MANY, paths.size());
        Sheet.confirm(ui, title, S.DELETE_DETAIL, S.DELETE, true, () -> {
            if (before != null) before.run();
            batch(paths, 1, "");
        });
    }
    void singleCopy(String from, String to) {
        request(3, S.WORKING_COPY, (r, t) -> {
            waitJob(r, r.startJob(0, from, to), t);
            ui.runOnUiThread(this::refresh);
        });
    }
    void waitJob(Remote r, String id, long t) throws Exception {
        try {
            for (;;) {
                if (epoch.get() != t)
                    throw new IOException("已取消");
                Remote.HostJob job = r.pollJob(id);
                String text = job.total > 0 ? String.format(Locale.ROOT, S.HOST_JOB_PROGRESS, job.current, job.total) : S.HOST_JOB_RUNNING;
                ui.runOnUiThread(() -> { if (epoch.get() == t) page().progress(text); });
                if (job.state == 4)
                    return;
                if (job.terminal())
                    throw new IOException(job.message);
                Thread.sleep(100);
            }
        } finally {
            if (epoch.get() != t)
                try (Remote control = new Remote(r.credentials, r.grantedScopes, 0)) {
                    control.cancelJob(id);
                } catch (Exception ignored) {
                }
        }
    }
    void batch(List<String> paths, int kind, String dest) {
        String label = kind == 1 ? S.WORKING_DELETE : kind == 3 ? S.WORKING_MOVE : S.WORKING_COPY;
        request(kind == 1 || kind == 3 ? 2 : 3, label, (r, t) -> {
            int done = 0;
            for (String from : paths) {
                if (epoch.get() != t)
                    return;
                String target = dest + "/" + from.substring(from.lastIndexOf('/') + 1);
                if (dest.isEmpty())
                    target = from.substring(from.lastIndexOf('/') + 1);
                if (kind == 3)
                    r.mutate(9, from, target);
                else
                    waitJob(r, r.startJob(kind, from, kind == 1 ? "" : target), t);
                done++;
                final String text = String.format(Locale.ROOT, S.BATCH_PROGRESS, label, done, paths.size());
                ui.runOnUiThread(() -> { if (epoch.get() == t) page().progress(text); });
            }
            ui.status(String.format(Locale.ROOT, S.BATCH_DONE, done));
            ui.runOnUiThread(this::refresh);
        });
    }
    public void close() {
        cancel();
        executor.shutdownNow();
    }
}
