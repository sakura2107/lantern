package org.lantern.client;

import android.os.SystemClock;
import android.text.TextUtils;
import android.view.*;
import android.widget.*;
import java.util.*;

/** Transfers page: queue summary and controls, filter, and one card per resumable task. */
final class TransfersPage {
    static final int ALL = 0, ACTIVE = 1, FINISHED = 2;

    /** Immutable snapshot of a queue task for one render pass. */
    static final class TaskItem {
        final String id, name, state, error;
        final int kind, retries;
        final boolean directory, otherDevice;
        final long current, total;
        double rate;
        TaskItem(TransferQueue.Task t, boolean other) {
            id = t.id; name = t.name; state = t.state; error = t.error; kind = t.kind; retries = t.retries;
            directory = t.directory; current = t.current; total = t.total; otherDevice = other;
        }
        boolean finished() { return state.equals("完成") || state.equals("已取消"); }
    }
    static final class Sample { long bytes, at; double rate; }

    final AppShell shell;
    final MainActivity a;
    final LinearLayout view, summary, filters;
    final TextView headline, detail;
    final Button limit, pauseAll, clear;
    final ListView list;
    final TextView[] filterTabs = new TextView[3];
    final LinearLayout emptyView;
    final Map<String, Sample> samples = new HashMap<>();
    List<TaskItem> all = new ArrayList<>(), shown = new ArrayList<>();
    final Adapter adapter = new Adapter();
    int filter = ALL;
    boolean anyPausable, anyResumable;

    TransfersPage(AppShell s) {
        shell = s;
        a = s.a;
        view = Ui.column(a);
        view.setPadding(Theme.dp(16), Theme.dp(4), Theme.dp(16), 0);

        summary = Ui.column(a);
        summary.setBackground(Ui.outline(Theme.SURFACE, Theme.LINE, 22));
        summary.setPadding(Theme.dp(18), Theme.dp(16), Theme.dp(18), Theme.dp(14));
        headline = Ui.numeric(Ui.medium(a, S.QUEUE_IDLE, 20, Theme.INK));
        headline.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        summary.addView(headline);
        detail = Ui.numeric(Ui.text(a, "", 13, Theme.MUTED));
        detail.setPadding(0, Theme.dp(2), 0, Theme.dp(12));
        detail.setLineSpacing(0, 1.15f);
        summary.addView(detail);
        LinearLayout buttons = Ui.row(a);
        limit = Ui.button(a, "", Ui.SECONDARY, this::showLimit);
        limit.setTextSize(13);
        limit.setPadding(Theme.dp(14), 0, Theme.dp(14), 0);
        Ui.id(limit, "transfers.limit");
        pauseAll = Ui.button(a, S.PAUSE_ALL, Ui.SECONDARY, this::toggleAll);
        pauseAll.setTextSize(13);
        pauseAll.setPadding(Theme.dp(14), 0, Theme.dp(14), 0);
        Ui.id(pauseAll, "transfers.pauseall");
        clear = Ui.button(a, S.CLEAR_DONE, Ui.SECONDARY, this::confirmClear);
        clear.setTextSize(13);
        clear.setPadding(Theme.dp(14), 0, Theme.dp(14), 0);
        Ui.id(clear, "transfers.clear");
        // Widths follow the labels; spare space is shared evenly.
        for (Button b : new Button[] {limit, pauseAll, clear}) { b.setMinWidth(0); b.setMinimumWidth(0); }
        buttons.addView(limit, new LinearLayout.LayoutParams(-2, Theme.dp(40), 1));
        LinearLayout.LayoutParams p2 = new LinearLayout.LayoutParams(-2, Theme.dp(40), 1);
        p2.setMarginStart(Theme.dp(8));
        buttons.addView(pauseAll, p2);
        LinearLayout.LayoutParams p3 = new LinearLayout.LayoutParams(-2, Theme.dp(40), 1);
        p3.setMarginStart(Theme.dp(8));
        buttons.addView(clear, p3);
        summary.addView(buttons, Ui.lp(-1, -2));
        view.addView(summary, Ui.lp(-1, -2));

        filters = Ui.row(a);
        filters.setPadding(0, Theme.dp(14), 0, Theme.dp(6));
        String[] labels = {S.FILTER_ALL, S.FILTER_ACTIVE, S.FILTER_DONE};
        for (int i = 0; i < 3; i++) {
            final int n = i;
            TextView t = Ui.medium(a, labels[i], 14, Theme.MUTED);
            t.setGravity(Gravity.CENTER);
            t.setPadding(Theme.dp(14), 0, Theme.dp(14), 0);
            t.setMinHeight(Theme.dp(36));
            t.setOnClickListener(v -> { filter = n; apply(); });
            t.setFocusable(true);
            Ui.role(t, RadioButton.class);
            Ui.id(t, "transfers.filter." + i);
            LinearLayout.LayoutParams fp = Ui.lp(-2, Theme.dp(36));
            fp.setMarginEnd(Theme.dp(6));
            filters.addView(t, fp);
            filterTabs[i] = t;
        }
        view.addView(filters, Ui.lp(-1, -2));

        FrameLayout area = new FrameLayout(a);
        list = new ListView(a);
        list.setDivider(new android.graphics.drawable.ColorDrawable(0));
        list.setDividerHeight(Theme.dp(10));
        list.setSelector(new android.graphics.drawable.ColorDrawable(0));
        list.setClipToPadding(false);
        list.setPadding(0, Theme.dp(2), 0, Theme.dp(80));
        list.setAdapter(adapter);
        Ui.id(list, "transfers.list");
        list.setOnItemClickListener((parent, row, index, id) -> showTask(shown.get(index)));
        area.addView(list, new FrameLayout.LayoutParams(-1, -1));
        emptyView = Ui.column(a);
        emptyView.setGravity(Gravity.CENTER);
        emptyView.setPadding(Theme.dp(32), 0, Theme.dp(32), Theme.dp(60));
        emptyView.addView(Ui.glyphTile(a, Glyph.TRANSFER, Theme.MUTED, Theme.INSET, 64, 22), Ui.lp(Theme.dp(64), Theme.dp(64)));
        TextView et = Ui.medium(a, S.NO_TASKS, 17, Theme.INK);
        et.setGravity(Gravity.CENTER);
        et.setPadding(0, Theme.dp(16), 0, Theme.dp(4));
        emptyView.addView(et);
        TextView ed = Ui.text(a, S.NO_TASKS_DETAIL, 14, Theme.MUTED);
        ed.setGravity(Gravity.CENTER);
        emptyView.addView(ed);
        Button go = Ui.button(a, S.GO_FILES, Ui.PRIMARY, () -> shell.select(AppShell.FILES));
        LinearLayout.LayoutParams gp = Ui.lp(-2, Theme.dp(44));
        gp.topMargin = Theme.dp(18);
        emptyView.addView(go, gp);
        area.addView(emptyView, new FrameLayout.LayoutParams(-1, -1));
        list.setEmptyView(emptyView);
        view.addView(area, new LinearLayout.LayoutParams(-1, 0, 1));
        apply();
    }

    /** Builds snapshots under the queue lock; returns the number of tasks still moving or waiting. */
    int render(TransferQueue q) {
        List<TaskItem> next = new ArrayList<>();
        long now = SystemClock.elapsedRealtime();
        synchronized (q) {
            for (TransferQueue.Task t : q.tasks)
                next.add(new TaskItem(t, q.credentials != null && !TransferQueue.matches(t, q.credentials)));
        }
        Set<String> alive = new HashSet<>();
        int moving = 0, waiting = 0, attention = 0;
        double totalRate = 0;
        anyPausable = false; anyResumable = false;
        for (TaskItem t : next) {
            alive.add(t.id);
            Sample sample = samples.get(t.id);
            if (sample == null) { sample = new Sample(); sample.bytes = t.current; sample.at = now; samples.put(t.id, sample); }
            if (t.state.equals("进行中")) {
                long dt = now - sample.at;
                if (dt >= 700) {
                    double instant = Math.max(0, t.current - sample.bytes) * 1000.0 / dt;
                    sample.rate = sample.rate == 0 ? instant : sample.rate * 0.6 + instant * 0.4;
                    sample.bytes = t.current; sample.at = now;
                }
                t.rate = sample.rate;
                totalRate += sample.rate;
                moving++;
            } else {
                sample.rate = 0; sample.bytes = t.current; sample.at = now;
            }
            if (t.state.equals("等待") || t.state.equals("等待网络")) waiting++;
            if (t.state.equals("等待处理")) attention++;
            if (!t.otherDevice && (t.state.equals("进行中") || t.state.equals("等待") || t.state.equals("等待网络"))) anyPausable = true;
            if (!t.otherDevice && t.state.equals("已暂停")) anyResumable = true;
        }
        samples.keySet().retainAll(alive);
        all = next;

        if (next.isEmpty()) headline.setText(S.QUEUE_IDLE);
        else if (moving > 0) headline.setText(String.format(Locale.ROOT, S.QUEUE_MOVING, moving + waiting));
        else if (attention > 0) headline.setText(String.format(Locale.ROOT, S.QUEUE_ATTENTION, attention));
        else if (waiting > 0) headline.setText(String.format(Locale.ROOT, S.QUEUE_WAITING, waiting));
        else headline.setText(S.QUEUE_QUIET);
        TransferPacer pacer = TransferPacer.SHARED;
        StringBuilder d = new StringBuilder();
        if (moving > 0) d.append(String.format(Locale.ROOT, S.QUEUE_RATE, Ui.rate(totalRate))).append('\n');
        d.append(String.format(Locale.ROOT, S.QUEUE_LIMIT, Ui.limit(pacer.rate())));
        if (pacer.latestRtt > 0) d.append(String.format(Locale.ROOT, S.QUEUE_RTT, pacer.latestRtt));
        if (pacer.realtime) d.append(S.QUEUE_REALTIME);
        detail.setText(d);
        // The effective cap is in the line above; the button stays short so all three fit at large font scales.
        limit.setText(S.LIMIT_SHORT);
        limit.setContentDescription(String.format(Locale.ROOT, S.LIMIT_BUTTON, Ui.limit(pacer.userLimit)));
        pauseAll.setText(anyPausable || !anyResumable ? S.PAUSE_ALL : S.RESUME_ALL);
        pauseAll.setEnabled(anyPausable || anyResumable);
        boolean finished = false;
        for (TaskItem t : next) if (t.state.equals("完成")) finished = true;
        clear.setEnabled(finished);
        apply();
        return moving + waiting;
    }

    void apply() {
        for (int i = 0; i < 3; i++) {
            boolean on = i == filter;
            filterTabs[i].setTextColor(on ? Theme.ON_INK : Theme.MUTED);
            filterTabs[i].setBackground(Ui.press(Ui.round(on ? Theme.INK : 0, 18), 18));
            filterTabs[i].setSelected(on);
        }
        List<TaskItem> next = new ArrayList<>();
        for (TaskItem t : all)
            if (filter == ALL || (filter == ACTIVE) != t.finished()) next.add(t);
        shown = next;
        adapter.notifyDataSetChanged();
    }

    static String label(String state) {
        switch (state) {
        case "进行中": return S.T_RUNNING;
        case "等待": return S.T_QUEUED;
        case "等待网络": return S.T_NETWORK;
        case "等待处理": return S.T_CONFLICT;
        case "已暂停": return S.T_PAUSED;
        case "完成": return S.T_DONE;
        case "已取消": return S.T_CANCELLED;
        default: return state;
        }
    }
    static int tone(String state) {
        switch (state) {
        case "进行中": return Theme.LAMP_TEXT;
        case "等待网络": return Theme.WARN;
        case "等待处理": return Theme.DANGER;
        case "完成": return Theme.POSITIVE;
        case "已取消": return Theme.FAINT;
        default: return Theme.MUTED;
        }
    }
    static int toneSoft(String state) {
        switch (state) {
        case "进行中": return Theme.LAMP_SOFT;
        case "等待网络": return Theme.WARN_SOFT;
        case "等待处理": return Theme.DANGER_SOFT;
        case "完成": return Theme.POSITIVE_SOFT;
        default: return Theme.INSET;
        }
    }

    // ---------- actions ----------
    TransferQueue.Task find(String id) {
        TransferQueue q = a.transfers;
        synchronized (q) { for (TransferQueue.Task t : q.tasks) if (t.id.equals(id)) return t; }
        return null;
    }

    void quick(TaskItem item) {
        TransferQueue.Task t = find(item.id);
        if (t == null) return;
        switch (item.state) {
        case "进行中": case "等待": case "等待网络": a.transfers.pause(t, false); break;
        case "已暂停": case "已取消": a.transfers.resume(t); break;
        case "等待处理": conflict(t); break;
        case "完成": a.transfers.openDone(t); break;
        default: return;
        }
        a.renderTransfers();
    }

    void showTask(TaskItem item) {
        TransferQueue.Task t = find(item.id);
        if (t == null) return;
        StringBuilder sub = new StringBuilder(label(item.state));
        sub.append("，").append(item.kind == 0 ? S.DIRECTION_DOWN : S.DIRECTION_UP);
        if (!item.error.isEmpty()) sub.append("\n").append(item.error);
        if (item.otherDevice) sub.append("\n").append(S.OTHER_DEVICE_TASK);
        Sheet sheet = new Sheet(a, item.name, sub);
        switch (item.state) {
        case "等待处理":
            sheet.item(Glyph.WARNING, S.RESOLVE_CONFLICT, S.RESOLVE_CONFLICT_DETAIL, () -> conflict(t));
            break;
        case "进行中": case "等待": case "等待网络":
            sheet.item(Glyph.PAUSE, S.PAUSE, S.PAUSE_DETAIL, () -> { a.transfers.pause(t, false); a.renderTransfers(); });
            break;
        case "已暂停": case "已取消":
            sheet.item(Glyph.PLAY, S.RESUME, S.RESUME_DETAIL, () -> { a.transfers.resume(t); a.renderTransfers(); });
            break;
        default:
        }
        if (item.state.equals("完成") && item.kind == 0 && !item.directory)
            sheet.item(Glyph.OPEN, S.OPEN_DONE, S.OPEN_DONE_DETAIL, () -> a.transfers.openDone(t));
        if (!item.finished())
            sheet.item(Glyph.TOP, S.MOVE_TOP, S.MOVE_TOP_DETAIL, () -> { a.transfers.moveToTop(t); a.renderTransfers(); });
        if (!item.finished())
            sheet.danger(Glyph.STOP, S.CANCEL_TASK, S.CANCEL_TASK_DETAIL, () -> { a.transfers.pause(t, true); a.renderTransfers(); });
        else
            sheet.danger(Glyph.TRASH, S.REMOVE_TASK, item.kind == 0 && item.state.equals("完成") ? S.REMOVE_TASK_DETAIL_CACHE : S.REMOVE_TASK_DETAIL,
                         () -> a.job(() -> { a.transfers.remove(t); a.runOnUiThread(a::renderTransfers); }));
        sheet.show();
    }

    void conflict(TransferQueue.Task t) {
        new Sheet(a, S.CONFLICT_TITLE, S.CONFLICT_DETAIL)
            .item(Glyph.EDIT, S.CONFLICT_RENAME, S.CONFLICT_RENAME_DETAIL, () ->
                Sheet.prompt(a, S.CONFLICT_NEW_NAME, null, S.CONFLICT_NEW_NAME, t.name, S.SAVE_AS, value -> {
                    try { a.transfers.renameConflict(t, value); } catch (Exception error) { a.status(error.getMessage()); }
                    a.renderTransfers();
                }))
            .item(Glyph.STOP, S.CONFLICT_SKIP, S.CONFLICT_SKIP_DETAIL, () -> { a.transfers.pause(t, true); a.renderTransfers(); })
            .show();
    }

    void toggleAll() {
        TransferQueue q = a.transfers;
        if (anyPausable) {
            q.pauseAll(S.PAUSED_BY_USER);
            a.status(S.PAUSED_ALL);
        } else {
            List<TransferQueue.Task> paused = new ArrayList<>();
            synchronized (q) {
                for (TransferQueue.Task t : q.tasks)
                    if (t.state.equals("已暂停") && (q.credentials == null || TransferQueue.matches(t, q.credentials))) paused.add(t);
            }
            for (TransferQueue.Task t : paused) q.resume(t);
            a.status(String.format(Locale.ROOT, S.RESUMED_N, paused.size()));
        }
        a.renderTransfers();
    }

    void confirmClear() {
        Sheet.confirm(a, S.CLEAR_TITLE, S.CLEAR_DETAIL, S.CLEAR_CONFIRM, true, () -> a.job(() -> {
            a.transfers.clearCompleted();
            a.runOnUiThread(a::renderTransfers);
        }));
    }

    void showLimit() {
        long[] mib = {1, 4, 16, 32};
        TransferPacer pacer = TransferPacer.SHARED;
        Sheet sheet = new Sheet(a, S.LIMIT_TITLE, String.format(Locale.ROOT, S.LIMIT_DETAIL, Ui.limit(pacer.rate())));
        for (long m : mib) {
            long bytes = m * 1024 * 1024;
            sheet.option(m + " MiB/s", m == 32 ? S.LIMIT_MAX : null, pacer.userLimit == bytes, () -> {
                pacer.userLimit = bytes;
                a.getSharedPreferences("device", 0).edit().putLong("transfer_limit", bytes).apply();
                a.renderTransfers();
            });
        }
        sheet.show();
    }

    final class Adapter extends BaseAdapter {
        public int getCount() { return shown.size(); }
        public Object getItem(int i) { return shown.get(i); }
        public long getItemId(int i) { return i; }
        public View getView(int i, View old, ViewGroup parent) {
            Card c = old instanceof Card ? (Card)old : new Card();
            c.bind(shown.get(i));
            return c;
        }
    }

    final class Card extends LinearLayout {
        final ImageView icon, action;
        final TextView name, state, amount, speed, error;
        final Ui.Meter meter;
        TaskItem item;
        Card() {
            super(a);
            setOrientation(VERTICAL);
            setPadding(Theme.dp(14), Theme.dp(12), Theme.dp(6), Theme.dp(12));
            LinearLayout top = Ui.row(a);
            icon = Ui.glyphTile(a, Glyph.DOWNLOAD, Theme.INK, Theme.INSET, 40, 13);
            top.addView(icon, Ui.lp(Theme.dp(40), Theme.dp(40)));
            LinearLayout lines = Ui.column(a);
            LinearLayout.LayoutParams lp = Ui.weight(1);
            lp.setMarginStart(Theme.dp(12));
            top.addView(lines, lp);
            name = Ui.text(a, "", 15, Theme.INK);
            name.setMaxLines(2);
            name.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            lines.addView(name);
            LinearLayout stateRow = Ui.row(a);
            stateRow.setPadding(0, Theme.dp(4), 0, 0);
            state = Ui.chip(a, "", Theme.MUTED, Theme.INSET);
            state.setTextSize(11);
            state.setPadding(Theme.dp(8), Theme.dp(2), Theme.dp(8), Theme.dp(2));
            stateRow.addView(state);
            amount = Ui.numeric(Ui.text(a, "", 12, Theme.MUTED));
            Ui.oneLine(amount);
            amount.setPadding(Theme.dp(8), 0, 0, 0);
            stateRow.addView(amount, Ui.weight(1));
            lines.addView(stateRow, Ui.lp(-1, -2));
            action = Ui.iconButton(a, Glyph.PAUSE, S.PAUSE, null);
            action.setFocusable(false);
            action.setOnClickListener(v -> { if (item != null) quick(item); });
            top.addView(action, Ui.lp(Theme.dp(44), Theme.dp(44)));
            addView(top, Ui.lp(-1, -2));
            meter = new Ui.Meter(a);
            LinearLayout.LayoutParams mp = Ui.lp(-1, Theme.dp(4));
            mp.setMargins(Theme.dp(52), Theme.dp(10), Theme.dp(10), 0);
            addView(meter, mp);
            speed = Ui.numeric(Ui.text(a, "", 12, Theme.MUTED));
            Ui.oneLine(speed);
            speed.setPadding(Theme.dp(52), Theme.dp(6), Theme.dp(10), 0);
            addView(speed);
            error = Ui.text(a, "", 12, Theme.WARN);
            error.setMaxLines(3);
            error.setEllipsize(TextUtils.TruncateAt.END);
            error.setPadding(Theme.dp(52), Theme.dp(4), Theme.dp(10), 0);
            addView(error);
            setLayoutParams(new AbsListView.LayoutParams(-1, -2));
        }
        void bind(TaskItem t) {
            item = t;
            setBackground(Ui.press(Ui.outline(Theme.SURFACE, Theme.LINE, 18), 18));
            setPadding(Theme.dp(14), Theme.dp(12), Theme.dp(6), Theme.dp(12));
            name.setText(t.name);
            icon.setImageDrawable(new Glyph(t.kind == 0 ? Glyph.DOWNLOAD : Glyph.UPLOAD, Theme.INK));
            state.setText(label(t.state));
            state.setTextColor(tone(t.state));
            state.setBackground(Ui.round(toneSoft(t.state), 10));
            String progress = t.kind == 2 ? String.format(Locale.ROOT, S.ITEMS_PROGRESS, t.current, t.total)
                                          : Ui.size(t.current) + " / " + Ui.size(t.total);
            amount.setText(progress);
            float fraction = t.total == 0 ? (t.state.equals("完成") ? 1 : 0) : (float)(t.current / (double)t.total);
            meter.set(fraction, t.state.equals("完成") ? Theme.POSITIVE : t.state.equals("进行中") ? Theme.LAMP : Theme.FAINT);
            meter.setVisibility(t.finished() && t.state.equals("已取消") ? View.GONE : View.VISIBLE);
            String sp = "";
            if (t.state.equals("进行中") && t.rate > 1) {
                sp = Ui.rate(t.rate);
                if (t.kind != 2 && t.total > t.current) sp += "，" + String.format(Locale.ROOT, S.ETA, Ui.duration(Math.round((t.total - t.current) / t.rate)));
            } else if (t.state.equals("等待网络")) {
                sp = String.format(Locale.ROOT, S.RETRYING, t.retries, RetryPolicy.MAX_ATTEMPTS);
            } else if (t.otherDevice) {
                sp = S.OTHER_DEVICE_TASK;
            }
            speed.setText(sp);
            speed.setVisibility(sp.isEmpty() ? View.GONE : View.VISIBLE);
            error.setText(t.error);
            error.setTextColor(t.state.equals("等待处理") ? Theme.DANGER : Theme.WARN);
            error.setVisibility(t.error.isEmpty() ? View.GONE : View.VISIBLE);
            int glyph; String desc;
            switch (t.state) {
            case "进行中": case "等待": case "等待网络": glyph = Glyph.PAUSE; desc = S.PAUSE; break;
            case "已暂停": case "已取消": glyph = Glyph.PLAY; desc = S.RESUME; break;
            case "等待处理": glyph = Glyph.WARNING; desc = S.RESOLVE_CONFLICT; break;
            case "完成": glyph = t.kind == 0 && !t.directory ? Glyph.OPEN : -1; desc = S.OPEN_DONE; break;
            default: glyph = -1; desc = "";
            }
            action.setVisibility(glyph < 0 ? View.INVISIBLE : View.VISIBLE);
            if (glyph >= 0) action.setImageDrawable(new Glyph(glyph, glyph == Glyph.WARNING ? Theme.DANGER : Theme.INK));
            action.setContentDescription(desc);
            setContentDescription(t.name + "，" + label(t.state) + "，" + progress);
        }
    }
}
