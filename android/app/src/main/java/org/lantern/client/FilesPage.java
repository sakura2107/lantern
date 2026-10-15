package org.lantern.client;

import android.text.Editable;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.view.*;
import android.widget.*;
import java.util.*;

/**
 * Files page: breadcrumb navigation, search/sort/select, the file list with per-item actions,
 * host-operation progress with cancel, paging and upload. File requests stay in FileBrowser.
 */
final class FilesPage {
    final AppShell shell;
    final MainActivity a;
    final LinearLayout view, crumbs, toolbar, selectionBar, selectionActions, banner, pager, emptyView;
    final HorizontalScrollView crumbScroll;
    final EditText path;
    final ListView list;
    final TextView selectionCount, bannerText, pagerText, emptyTitle, emptyDetail;
    final Button emptyAction, upload, prev, next;
    final ImageView emptyIcon;
    final Ui.Meter meter;
    final ProgressBar bannerSpinner;
    List<Remote.Entry> items = new ArrayList<>();
    String searchQuery;
    boolean loading, noReadPermission;
    /** Reason the last listing failed, or null. Cleared by the next successful listing. */
    String loadError;
    int total, offset;

    FilesPage(AppShell s) {
        shell = s;
        a = s.a;
        view = Ui.column(a);

        // Path is the single source of truth for the current directory (relative to the host root).
        path = new EditText(a);
        path.setVisibility(View.GONE);
        view.addView(path);

        LinearLayout crumbRow = Ui.row(a);
        crumbRow.setPadding(Theme.dp(12), 0, Theme.dp(8), 0);
        crumbScroll = new HorizontalScrollView(a);
        crumbScroll.setHorizontalScrollBarEnabled(false);
        crumbs = Ui.row(a);
        crumbScroll.addView(crumbs);
        crumbRow.addView(crumbScroll, Ui.weight(1));
        ImageView more = Ui.iconButton(a, Glyph.MORE, S.MORE, this::showMore);
        Ui.id(more, "files.more");
        crumbRow.addView(more, Ui.lp(Theme.dp(44), Theme.dp(44)));
        view.addView(crumbRow, Ui.lp(-1, -2));

        toolbar = Ui.row(a);
        toolbar.setPadding(Theme.dp(16), Theme.dp(2), Theme.dp(10), Theme.dp(8));
        LinearLayout search = Ui.row(a);
        search.setPadding(Theme.dp(14), 0, Theme.dp(14), 0);
        search.setBackground(Ui.press(Ui.round(Theme.INSET, 22), 22));
        ImageView searchIcon = new ImageView(a);
        searchIcon.setImageDrawable(new Glyph(Glyph.SEARCH, Theme.MUTED, Theme.dp(18)));
        search.addView(searchIcon, Ui.lp(Theme.dp(18), Theme.dp(18)));
        TextView searchHint = Ui.text(a, S.SEARCH_HINT, 15, Theme.FAINT);
        Ui.oneLine(searchHint);
        searchHint.setPadding(Theme.dp(10), 0, 0, 0);
        search.addView(searchHint, Ui.weight(1));
        search.setOnClickListener(v -> promptSearch());
        search.setFocusable(true);
        search.setContentDescription(S.SEARCH);
        Ui.role(search, Button.class);
        Ui.id(search, "files.search");
        toolbar.addView(search, new LinearLayout.LayoutParams(0, Theme.dp(44), 1));
        ImageView sort = Ui.iconButton(a, Glyph.SORT, S.SORT, this::showSort);
        Ui.id(sort, "files.sort");
        LinearLayout.LayoutParams ip = Ui.lp(Theme.dp(44), Theme.dp(44));
        ip.setMarginStart(Theme.dp(4));
        toolbar.addView(sort, ip);
        ImageView select = Ui.iconButton(a, Glyph.SELECT, S.SELECT, () -> setSelecting(true));
        Ui.id(select, "files.select");
        toolbar.addView(select, Ui.lp(Theme.dp(44), Theme.dp(44)));
        view.addView(toolbar, Ui.lp(-1, -2));

        selectionBar = Ui.row(a);
        selectionBar.setPadding(Theme.dp(8), Theme.dp(2), Theme.dp(12), Theme.dp(8));
        selectionBar.setMinimumHeight(Theme.dp(52));
        ImageView close = Ui.iconButton(a, Glyph.CLOSE, S.EXIT_SELECTION, () -> setSelecting(false));
        Ui.id(close, "files.select.exit");
        selectionBar.addView(close, Ui.lp(Theme.dp(44), Theme.dp(44)));
        selectionCount = Ui.numeric(Ui.medium(a, "", 17, Theme.INK));
        selectionCount.setPadding(Theme.dp(6), 0, 0, 0);
        selectionBar.addView(selectionCount, Ui.weight(1));
        Button all = Ui.button(a, S.SELECT_ALL, Ui.GHOST, this::selectAll);
        Ui.id(all, "files.select.all");
        selectionBar.addView(all, Ui.lp(-2, Theme.dp(40)));
        selectionBar.setVisibility(View.GONE);
        view.addView(selectionBar, Ui.lp(-1, -2));

        banner = Ui.row(a);
        banner.setPadding(Theme.dp(14), Theme.dp(8), Theme.dp(8), Theme.dp(8));
        banner.setBackground(Ui.round(Theme.LAMP_SOFT, 16));
        bannerSpinner = Ui.spinner(a, 18);
        banner.addView(bannerSpinner);
        bannerText = Ui.numeric(Ui.text(a, "", 14, Theme.INK));
        bannerText.setMaxLines(2);
        bannerText.setEllipsize(TextUtils.TruncateAt.END);
        bannerText.setPadding(Theme.dp(10), 0, Theme.dp(8), 0);
        banner.addView(bannerText, Ui.weight(1));
        Button cancel = Ui.button(a, S.CANCEL, Ui.GHOST, () -> { a.browser.cancel(); a.status(S.CANCEL_REQUESTED); });
        Ui.id(cancel, "files.cancel");
        banner.addView(cancel, Ui.lp(-2, Theme.dp(40)));
        banner.setVisibility(View.GONE);
        LinearLayout.LayoutParams bp = Ui.lp(-1, -2);
        bp.setMargins(Theme.dp(16), 0, Theme.dp(16), Theme.dp(8));
        view.addView(banner, bp);

        meter = new Ui.Meter(a);
        meter.setVisibility(View.INVISIBLE);
        LinearLayout.LayoutParams mp = Ui.lp(-1, Theme.dp(3));
        mp.setMargins(Theme.dp(16), 0, Theme.dp(16), Theme.dp(2));
        view.addView(meter, mp);

        FrameLayout area = new FrameLayout(a);
        list = new ListView(a);
        list.setDivider(null);
        list.setSelector(new android.graphics.drawable.ColorDrawable(0));
        list.setClipToPadding(false);
        list.setPadding(Theme.dp(10), Theme.dp(2), Theme.dp(10), Theme.dp(88));
        list.setScrollBarStyle(View.SCROLLBARS_OUTSIDE_OVERLAY);
        Ui.id(list, "files.list");
        area.addView(list, new FrameLayout.LayoutParams(-1, -1));

        emptyView = Ui.column(a);
        emptyView.setGravity(Gravity.CENTER);
        emptyView.setPadding(Theme.dp(32), 0, Theme.dp(32), Theme.dp(60));
        emptyIcon = Ui.glyphTile(a, Glyph.FOLDER, Theme.MUTED, Theme.INSET, 64, 22);
        emptyView.addView(emptyIcon, Ui.lp(Theme.dp(64), Theme.dp(64)));
        emptyTitle = Ui.medium(a, "", 17, Theme.INK);
        emptyTitle.setGravity(Gravity.CENTER);
        emptyTitle.setPadding(0, Theme.dp(16), 0, Theme.dp(4));
        emptyView.addView(emptyTitle);
        emptyDetail = Ui.text(a, "", 14, Theme.MUTED);
        emptyDetail.setGravity(Gravity.CENTER);
        emptyDetail.setLineSpacing(0, 1.15f);
        emptyView.addView(emptyDetail);
        emptyAction = Ui.button(a, "", Ui.PRIMARY, null);
        LinearLayout.LayoutParams eap = Ui.lp(-2, Theme.dp(44));
        eap.topMargin = Theme.dp(18);
        emptyView.addView(emptyAction, eap);
        area.addView(emptyView, new FrameLayout.LayoutParams(-1, -1));
        list.setEmptyView(emptyView);

        upload = Ui.button(a, S.UPLOAD, Ui.PRIMARY, this::showUpload);
        Ui.withIcon(upload, Glyph.UPLOAD);
        upload.setElevation(Theme.dp(4));
        upload.setPadding(Theme.dp(18), 0, Theme.dp(22), 0);
        Ui.id(upload, "files.upload");
        FrameLayout.LayoutParams up = new FrameLayout.LayoutParams(-2, Theme.dp(52), Gravity.BOTTOM | Gravity.END);
        up.setMargins(0, 0, Theme.dp(16), Theme.dp(16));
        area.addView(upload, up);
        view.addView(area, new LinearLayout.LayoutParams(-1, 0, 1));

        pager = Ui.row(a);
        pager.setPadding(Theme.dp(20), Theme.dp(6), Theme.dp(12), Theme.dp(6));
        pager.setBackgroundColor(Theme.BG);
        pagerText = Ui.numeric(Ui.text(a, "", 13, Theme.MUTED));
        pager.addView(pagerText, Ui.weight(1));
        prev = Ui.button(a, S.PREV_PAGE, Ui.GHOST, () -> { a.offset = Math.max(0, a.offset - 256); a.browser.refresh(); });
        next = Ui.button(a, S.NEXT_PAGE, Ui.GHOST, a::nextFiles);
        Ui.id(prev, "files.prev");
        Ui.id(next, "files.next");
        pager.addView(prev, Ui.lp(-2, Theme.dp(40)));
        pager.addView(next, Ui.lp(-2, Theme.dp(40)));
        pager.setVisibility(View.GONE);
        view.addView(pager, Ui.lp(-1, -2));

        selectionActions = Ui.row(a);
        selectionActions.setBackgroundColor(Theme.SURFACE);
        selectionActions.setPadding(Theme.dp(6), Theme.dp(4), Theme.dp(6), Theme.dp(4));
        bulk(Glyph.DOWNLOAD, S.DOWNLOAD, this::downloadSelected, "files.bulk.download");
        bulk(Glyph.COPY, S.COPY_TO, () -> copyOrMove(false), "files.bulk.copy");
        bulk(Glyph.MOVE, S.MOVE_TO, () -> copyOrMove(true), "files.bulk.move");
        bulk(Glyph.TRASH, S.DELETE, this::deleteSelected, "files.bulk.delete");
        selectionActions.setVisibility(View.GONE);
        view.addView(Ui.hairline(a), Ui.lp(-1, 0));
        view.addView(selectionActions, Ui.lp(-1, -2));

        path.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence c, int x, int y, int z) {}
            public void onTextChanged(CharSequence c, int x, int y, int z) {}
            public void afterTextChanged(Editable e) { searchQuery = null; renderCrumbs(); }
        });
        list.setOnItemClickListener((parent, row, index, id) -> click(index));
        list.setOnItemLongClickListener((parent, row, index, id) -> {
            if (!a.browser.selecting) { setSelecting(true); list.setItemChecked(index, true); click(index); }
            return true;
        });
        renderCrumbs();
        updateEmpty();
    }

    void bulk(int glyph, String label, Runnable action, String id) {
        LinearLayout b = Ui.column(a);
        b.setGravity(Gravity.CENTER);
        b.setPadding(0, Theme.dp(6), 0, Theme.dp(6));
        ImageView icon = new ImageView(a);
        icon.setImageDrawable(new Glyph(glyph, glyph == Glyph.TRASH ? Theme.DANGER : Theme.INK));
        b.addView(icon, Ui.lp(Theme.dp(22), Theme.dp(22)));
        TextView t = Ui.text(a, label, 12, glyph == Glyph.TRASH ? Theme.DANGER : Theme.INK);
        t.setPadding(0, Theme.dp(4), 0, 0);
        b.addView(t);
        b.setBackground(Ui.press(Ui.round(0, 14), 14));
        b.setOnClickListener(v -> action.run());
        b.setFocusable(true);
        b.setContentDescription(label);
        Ui.role(b, Button.class);
        Ui.id(b, id);
        selectionActions.addView(b, new LinearLayout.LayoutParams(0, Theme.dp(60), 1));
    }

    String directory() { return path.getText().toString(); }

    void navigate(String dir) {
        if (a.browser.selecting) setSelecting(false);
        path.setText(dir);
        a.offset = 0;
        a.browser.refresh();
    }

    void renderCrumbs() {
        crumbs.removeAllViews();
        if (searchQuery != null) {
            TextView chip = Ui.medium(a, String.format(Locale.ROOT, S.SEARCH_RESULTS_FOR, searchQuery), 15, Theme.INK);
            Ui.oneLine(chip);
            chip.setMaxWidth(Theme.dp(220));
            chip.setPadding(Theme.dp(8), Theme.dp(10), Theme.dp(4), Theme.dp(10));
            crumbs.addView(chip);
            ImageView clear = Ui.iconButton(a, Glyph.CLOSE, S.CLEAR_SEARCH, () -> { searchQuery = null; a.offset = 0; a.browser.refresh(); renderCrumbs(); });
            Ui.id(clear, "files.search.clear");
            crumbs.addView(clear, Ui.lp(Theme.dp(40), Theme.dp(40)));
            return;
        }
        String dir = directory();
        List<String> parts = new ArrayList<>();
        for (String part : dir.split("/")) if (!part.isEmpty()) parts.add(part);
        crumb(S.ROOT_FOLDER, "", parts.isEmpty(), true);
        StringBuilder acc = new StringBuilder();
        for (int i = 0; i < parts.size(); i++) {
            if (acc.length() > 0) acc.append('/');
            acc.append(parts.get(i));
            ImageView sep = new ImageView(a);
            sep.setImageDrawable(new Glyph(Glyph.CHEVRON_RIGHT, Theme.FAINT, Theme.dp(16)));
            crumbs.addView(sep, Ui.lp(Theme.dp(16), Theme.dp(16)));
            crumb(parts.get(i), acc.toString(), i == parts.size() - 1, false);
        }
        crumbScroll.post(() -> crumbScroll.fullScroll(View.FOCUS_RIGHT));
    }

    void crumb(String label, String target, boolean current, boolean home) {
        TextView t = Ui.text(a, label, 15, current ? Theme.INK : Theme.MUTED);
        if (current) t.setTypeface(Theme.MEDIUM);
        Ui.oneLine(t);
        t.setMaxWidth(Theme.dp(180));
        t.setGravity(Gravity.CENTER_VERTICAL);
        t.setMinHeight(Theme.dp(44));
        t.setPadding(Theme.dp(8), 0, Theme.dp(8), 0);
        if (home) {
            Glyph g = new Glyph(Glyph.HOME, current ? Theme.INK : Theme.MUTED, Theme.dp(18));
            g.setBounds(0, 0, g.size, g.size);
            t.setCompoundDrawablesRelative(g, null, null, null);
            t.setCompoundDrawablePadding(Theme.dp(6));
        }
        if (!current) {
            t.setBackground(Ui.press(Ui.round(0, 12), 12));
            t.setOnClickListener(v -> navigate(target));
            Ui.role(t, Button.class);
        } else if (!home) {
            t.setContentDescription(String.format(Locale.ROOT, S.CURRENT_FOLDER, label));
        }
        Ui.id(t, home ? "files.crumb.root" : "files.crumb");
        crumbs.addView(t);
    }

    // ---------- list ----------
    void render(List<Remote.Entry> data, boolean selecting) {
        items = data;
        list.setAdapter(new Adapter());
        list.setChoiceMode(selecting ? ListView.CHOICE_MODE_MULTIPLE : ListView.CHOICE_MODE_NONE);
        updateSelection();
        updateEmpty();
    }

    void pageInfo(int totalCount, int start, int shown, String query) {
        total = totalCount;
        offset = start;
        searchQuery = query;
        renderCrumbs();
        boolean paged = query == null && (start > 0 || totalCount > start + shown);
        pager.setVisibility(paged ? View.VISIBLE : View.GONE);
        pagerText.setText(shown == 0 ? String.format(Locale.ROOT, S.PAGE_EMPTY, totalCount)
                                     : String.format(Locale.ROOT, S.PAGE_RANGE, start + 1, start + shown, totalCount));
        prev.setEnabled(start > 0);
        next.setEnabled(totalCount > start + shown);
    }

    void searchProgress(int found, long scanned, boolean done) {
        if (!done) bannerText.setText(String.format(Locale.ROOT, S.SEARCHING, found, scanned));
    }

    void click(int index) {
        if (index < 0 || index >= items.size()) return;
        Remote.Entry e = items.get(index);
        if (a.browser.selecting) {
            String chosen = a.browser.resolve(e.name);
            if (list.isItemChecked(index)) a.browser.selected.add(chosen);
            else a.browser.selected.remove(chosen);
            updateSelection();
            return;
        }
        if (e.directory) navigate(a.browser.resolve(e.name));
        else showFile(e);
    }

    void setSelecting(boolean on) {
        if (on && items.isEmpty()) { a.status(S.NOTHING_TO_SELECT); return; }
        a.browser.selecting = on;
        a.browser.selected.clear();
        list.clearChoices();
        list.setChoiceMode(on ? ListView.CHOICE_MODE_MULTIPLE : ListView.CHOICE_MODE_NONE);
        if (list.getAdapter() instanceof BaseAdapter) ((BaseAdapter)list.getAdapter()).notifyDataSetChanged();
        updateSelection();
    }

    void selectAll() {
        boolean everything = a.browser.selected.size() == items.size();
        a.browser.selected.clear();
        for (int i = 0; i < items.size(); i++) {
            list.setItemChecked(i, !everything);
            if (!everything) a.browser.selected.add(a.browser.resolve(items.get(i).name));
        }
        updateSelection();
    }

    void updateSelection() {
        boolean on = a.browser != null && a.browser.selecting;
        toolbar.setVisibility(on ? View.GONE : View.VISIBLE);
        selectionBar.setVisibility(on ? View.VISIBLE : View.GONE);
        selectionActions.setVisibility(on ? View.VISIBLE : View.GONE);
        upload.setVisibility(on ? View.GONE : View.VISIBLE);
        if (on) {
            int n = a.browser.selected.size();
            selectionCount.setText(n == 0 ? S.SELECT_ITEMS : String.format(Locale.ROOT, S.SELECTED_N, n));
            for (int i = 0; i < selectionActions.getChildCount(); i++) {
                selectionActions.getChildAt(i).setEnabled(n > 0);
                selectionActions.getChildAt(i).setAlpha(n > 0 ? 1f : 0.4f);
            }
        }
    }

    List<String> selectedPaths() { return new ArrayList<>(a.browser.selected); }

    void downloadSelected() {
        int queued = 0;
        for (Remote.Entry item : items)
            if (a.browser.selected.contains(a.browser.resolve(item.name))) {
                a.transfers.add(a.browser.resolve(item.name), Ui.baseName(item.name), item.directory, 0, "");
                queued++;
            }
        setSelecting(false);
        if (queued > 0) a.status(String.format(Locale.ROOT, S.QUEUED_DOWNLOADS, queued));
    }

    void copyOrMove(boolean move) {
        List<String> paths = selectedPaths();
        Sheet.prompt(a, move ? S.MOVE_TITLE : S.COPY_TITLE, String.format(Locale.ROOT, S.DEST_HELP, paths.size()),
                     S.DEST_HINT, directory(), move ? S.MOVE_CONFIRM : S.COPY_CONFIRM, dest -> {
            setSelecting(false);
            a.browser.batch(paths, move ? 3 : 0, trimPath(dest));
        });
    }

    void deleteSelected() {
        List<String> paths = selectedPaths();
        a.browser.confirmDelete(paths, () -> setSelecting(false));
    }

    // ---------- sheets ----------
    void showFile(Remote.Entry e) {
        String remote = a.browser.resolve(e.name), name = Ui.baseName(e.name);
        String meta = Ui.size(e.size) + (e.modified > 0 ? "，" + String.format(Locale.ROOT, S.MODIFIED_AT, Ui.fullDate(e.modified)) : "");
        Sheet sheet = new Sheet(a, name, meta);
        sheet.item(Glyph.OPEN, S.OPEN_WITH_APP, S.OPEN_WITH_APP_DETAIL, () -> a.openRemote(e, remote, 0));
        sheet.item(Glyph.DOWNLOAD, S.DOWNLOAD_OPEN, S.DOWNLOAD_OPEN_DETAIL, () -> a.openRemote(e, remote, 1));
        sheet.item(Glyph.TRANSFER, S.QUEUE_DOWNLOAD, S.QUEUE_DOWNLOAD_DETAIL, () -> a.openRemote(e, remote, 2));
        manage(sheet, e, remote);
        sheet.show();
    }

    void showActions(Remote.Entry e) {
        String remote = a.browser.resolve(e.name);
        if (!e.directory) { showFile(e); return; }
        Sheet sheet = new Sheet(a, Ui.baseName(e.name), e.modified > 0 ? String.format(Locale.ROOT, S.MODIFIED_AT, Ui.fullDate(e.modified)) : S.FOLDER);
        sheet.item(Glyph.FOLDER, S.OPEN_FOLDER, null, () -> navigate(remote));
        sheet.item(Glyph.DOWNLOAD, S.DOWNLOAD_FOLDER, S.DOWNLOAD_FOLDER_DETAIL, () -> a.transfers.add(remote, Ui.baseName(e.name), true, 0, ""));
        manage(sheet, e, remote);
        sheet.show();
    }

    void manage(Sheet sheet, Remote.Entry e, String remote) {
        sheet.section(S.MANAGE);
        if (Glyph.isImage(e.name)) sheet.item(Glyph.IMAGE, S.PREVIEW_THUMB, null, () -> a.browser.thumbnail(e, remote));
        sheet.item(Glyph.EDIT, S.RENAME_MOVE, S.RENAME_MOVE_DETAIL, () -> a.browser.rename(remote));
        sheet.item(Glyph.COPY, S.HOST_COPY, S.HOST_COPY_DETAIL, () -> a.browser.copyOne(remote));
        sheet.item(Glyph.INFO, S.DETAILS, null, () -> a.browser.info(e, remote));
        sheet.danger(Glyph.TRASH, S.DELETE, e.directory ? S.DELETE_FOLDER_DETAIL : null,
                     () -> a.browser.confirmDelete(Collections.singletonList(remote), null));
    }

    void showUpload() {
        if (a.credentials == null) { a.status(S.NEED_DEVICE); shell.select(AppShell.DEVICE); return; }
        String target = directory().isEmpty() ? S.ROOT_FOLDER : directory();
        new Sheet(a, S.UPLOAD_TITLE, String.format(Locale.ROOT, S.UPLOAD_TO, target))
            .item(Glyph.FILE, S.UPLOAD_FILES, S.UPLOAD_FILES_DETAIL, a::uploadFiles)
            .item(Glyph.FOLDER, S.UPLOAD_FOLDER, S.UPLOAD_FOLDER_DETAIL, a::uploadDirectory)
            .show();
    }

    void showSort() {
        String[] labels = {S.SORT_NAME_ASC, S.SORT_NAME_DESC, S.SORT_SIZE_ASC, S.SORT_SIZE_DESC, S.SORT_TIME_ASC, S.SORT_TIME_DESC};
        Sheet sheet = new Sheet(a, S.SORT, null);
        int current = a.browser.sort * 2 + (a.browser.descending ? 1 : 0);
        for (int i = 0; i < labels.length; i++) {
            final int n = i;
            sheet.option(labels[i], null, i == current, () -> {
                a.browser.sort = n / 2;
                a.browser.descending = n % 2 != 0;
                a.offset = 0;
                a.browser.refresh();
            });
        }
        sheet.show();
    }

    void showMore() {
        new Sheet(a, S.FOLDER_ACTIONS, null)
            .item(Glyph.FOLDER_PLUS, S.NEW_FOLDER, null, a.browser::newFolder)
            .item(Glyph.REFRESH, S.REFRESH, null, () -> { a.offset = 0; a.browser.refresh(); })
            .item(Glyph.CHEVRON_RIGHT, S.GO_TO_PATH, S.GO_TO_PATH_DETAIL, () ->
                Sheet.prompt(a, S.GO_TO_PATH, null, S.HINT_PATH, directory(), S.GO, value -> navigate(trimPath(value))))
            .item(Glyph.SELECT, S.SELECT, null, () -> setSelecting(true))
            .show();
    }

    static String trimPath(String value) {
        String v = value.replace('\\', '/').trim();
        while (v.startsWith("/")) v = v.substring(1);
        while (v.endsWith("/")) v = v.substring(0, v.length() - 1);
        return v;
    }

    void promptSearch() {
        if (a.credentials == null) { a.status(S.NEED_DEVICE); return; }
        Sheet.prompt(a, S.SEARCH, String.format(Locale.ROOT, S.SEARCH_SCOPE, directory().isEmpty() ? S.ROOT_FOLDER : directory()),
                     S.SEARCH_HINT, "", S.SEARCH, query -> a.browser.search(query));
    }

    // ---------- status ----------
    void loading(boolean on, String label) {
        loading = on;
        meter.busy(on && label == null);
        meter.setVisibility(on && label == null ? View.VISIBLE : View.INVISIBLE);
        banner.setVisibility(on && label != null ? View.VISIBLE : View.GONE);
        if (label != null) bannerText.setText(label);
        updateEmpty();
    }
    void progress(String text) { if (banner.getVisibility() == View.VISIBLE) bannerText.setText(text); }

    void updateEmpty() {
        emptyAction.setVisibility(View.GONE);
        if (a.credentials == null) {
            empty(Glyph.PHONE, S.EMPTY_NO_DEVICE, S.EMPTY_NO_DEVICE_DETAIL);
            emptyAction.setText(S.GO_CONNECT);
            emptyAction.setOnClickListener(v -> shell.select(AppShell.DEVICE));
            emptyAction.setVisibility(View.VISIBLE);
        } else if (loading && items.isEmpty()) {
            empty(Glyph.FOLDER, S.EMPTY_LOADING, "");
        } else if (loadError != null) {
            // A failed listing must not look like an empty folder.
            empty(Glyph.WARNING, S.EMPTY_LOAD_FAILED, loadError);
            emptyAction.setText(S.RETRY);
            emptyAction.setOnClickListener(v -> { a.offset = 0; a.browser.refresh(); });
            emptyAction.setVisibility(View.VISIBLE);
        } else if (noReadPermission) {
            empty(Glyph.LOCK, S.EMPTY_NO_PERMISSION, S.EMPTY_NO_PERMISSION_DETAIL);
        } else if (searchQuery != null) {
            empty(Glyph.SEARCH, S.EMPTY_NO_RESULTS, S.EMPTY_NO_RESULTS_DETAIL);
        } else if (!shell.connected && items.isEmpty()) {
            empty(Glyph.FOLDER, S.EMPTY_NOT_LOADED, S.EMPTY_NOT_LOADED_DETAIL);
            emptyAction.setText(S.LOAD_FILES);
            emptyAction.setOnClickListener(v -> { a.offset = 0; a.browser.refresh(); });
            emptyAction.setVisibility(View.VISIBLE);
        } else {
            empty(Glyph.FOLDER, S.EMPTY_FOLDER, S.EMPTY_FOLDER_DETAIL);
        }
    }
    void empty(int glyph, String title, String detail) {
        emptyIcon.setImageDrawable(new Glyph(glyph, Theme.MUTED));
        emptyTitle.setText(title);
        emptyDetail.setText(detail);
        emptyDetail.setVisibility(detail.isEmpty() ? View.GONE : View.VISIBLE);
    }

    final class Adapter extends BaseAdapter {
        public int getCount() { return items.size(); }
        public Object getItem(int i) { return items.get(i); }
        public long getItemId(int i) { return i; }
        public View getView(int i, View recycled, ViewGroup parent) {
            Row r = recycled instanceof Row ? (Row)recycled : new Row();
            r.bind(items.get(i), i);
            return r;
        }
    }

    final class Row extends LinearLayout implements Checkable {
        final ImageView icon, trailing;
        final TextView name, meta;
        boolean checked;
        Remote.Entry entry;
        Row() {
            super(a);
            setGravity(Gravity.CENTER_VERTICAL);
            setPadding(Theme.dp(10), Theme.dp(8), Theme.dp(4), Theme.dp(8));
            setMinimumHeight(Theme.dp(64));
            icon = Ui.glyphTile(a, Glyph.FILE, Theme.INK, Theme.INSET, 44, 14);
            addView(icon, Ui.lp(Theme.dp(44), Theme.dp(44)));
            LinearLayout lines = Ui.column(a);
            LinearLayout.LayoutParams p = Ui.weight(1);
            p.setMarginStart(Theme.dp(14));
            p.setMarginEnd(Theme.dp(4));
            addView(lines, p);
            name = Ui.text(a, "", 16, Theme.INK);
            name.setMaxLines(2);
            name.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            lines.addView(name);
            meta = Ui.numeric(Ui.text(a, "", 13, Theme.MUTED));
            Ui.oneLine(meta);
            meta.setPadding(0, Theme.dp(2), 0, 0);
            lines.addView(meta);
            trailing = Ui.iconButton(a, Glyph.MORE, S.MORE_ACTIONS, null);
            trailing.setOnClickListener(v -> { if (entry != null && !a.browser.selecting) showActions(entry); });
            trailing.setFocusable(false); // a focusable child would swallow ListView item clicks
            addView(trailing, Ui.lp(Theme.dp(44), Theme.dp(44)));
            setBackground(Ui.press(Ui.round(0, 16), 16));
        }
        void bind(Remote.Entry e, int index) {
            entry = e;
            name.setText(e.name);
            icon.setImageDrawable(new Glyph(Glyph.forFile(e.name, e.directory), Theme.INK));
            String date = Ui.shortDate(e.modified);
            meta.setText(e.directory ? (date.isEmpty() ? S.FOLDER : S.FOLDER + "    " + date)
                                     : (date.isEmpty() ? Ui.size(e.size) : Ui.size(e.size) + "    " + date));
            setContentDescription((e.directory ? S.FOLDER : S.FILE) + "，" + e.name);
            setChecked(list.isItemChecked(index));
        }
        public boolean isChecked() { return checked; }
        public void toggle() { setChecked(!checked); }
        public void setChecked(boolean value) {
            checked = value;
            boolean selecting = a.browser != null && a.browser.selecting;
            if (selecting) {
                trailing.setImageDrawable(new Glyph(value ? Glyph.CHECK : Glyph.SELECT, value ? Theme.ON_INK : Theme.FAINT));
                trailing.setBackground(value ? Ui.round(Theme.INK, 22) : null);
                trailing.setClickable(false);
                trailing.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
                setBackground(Ui.press(Ui.round(value ? Theme.INSET : 0, 16), 16));
            } else {
                trailing.setImageDrawable(new Glyph(Glyph.MORE, Theme.MUTED));
                trailing.setBackground(Ui.pressOval(0));
                trailing.setClickable(true);
                trailing.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_AUTO);
                setBackground(Ui.press(Ui.round(0, 16), 16));
            }
        }
    }
}
