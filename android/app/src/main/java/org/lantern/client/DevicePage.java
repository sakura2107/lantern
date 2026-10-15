package org.lantern.client;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.*;
import android.widget.*;
import java.util.Locale;

/** Device page: the connection lamp, pairing details and connection tools. */
final class DevicePage {
    final AppShell shell;
    final MainActivity a;
    final ScrollView view;
    final Lamp lamp;
    final TextView headline, detail, fingerprintSummary, pinHint, formSummary;
    final Button scan, pair, browse, disconnect;
    final EditText host, pin, secret;
    final LinearLayout form, formHeader;
    final ImageView formChevron;
    final TextView notifyState;
    boolean formOpen = true, secretVisible;

    DevicePage(AppShell s) {
        shell = s;
        a = s.a;
        view = new ScrollView(a);
        view.setFillViewport(true);
        view.setClipToPadding(false);
        LinearLayout content = Ui.column(a);
        content.setPadding(Theme.dp(16), Theme.dp(6), Theme.dp(16), Theme.dp(96));
        view.addView(content);

        // Hero: the lamp and what the app can do right now.
        LinearLayout hero = Ui.column(a);
        hero.setBackground(Ui.outline(Theme.SURFACE, Theme.LINE, 24));
        hero.setPadding(Theme.dp(20), Theme.dp(20), Theme.dp(20), Theme.dp(20));
        content.addView(hero, Ui.lp(-1, -2));
        LinearLayout top = Ui.row(a);
        lamp = new Lamp(a);
        top.addView(lamp, Ui.lp(Theme.dp(64), Theme.dp(64)));
        LinearLayout lines = Ui.column(a);
        LinearLayout.LayoutParams lp = Ui.weight(1);
        lp.setMarginStart(Theme.dp(14));
        top.addView(lines, lp);
        headline = Ui.medium(a, S.HERO_UNPAIRED, 20, Theme.INK);
        headline.setMaxLines(2);
        headline.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        lines.addView(headline);
        fingerprintSummary = Ui.numeric(Ui.text(a, "", 13, Theme.MUTED));
        Ui.oneLine(fingerprintSummary);
        fingerprintSummary.setPadding(0, Theme.dp(2), 0, 0);
        lines.addView(fingerprintSummary);
        hero.addView(top, Ui.lp(-1, -2));
        detail = Ui.text(a, S.HERO_UNPAIRED_DETAIL, 14, Theme.MUTED);
        detail.setLineSpacing(0, 1.15f);
        detail.setPadding(0, Theme.dp(14), 0, Theme.dp(16));
        hero.addView(detail);
        LinearLayout actions = Ui.row(a);
        // First pairing: scanning the computer's QR is the main path; manual entry stays below.
        scan = Ui.button(a, S.SCAN_PAIR, Ui.PRIMARY, a::scanPairing);
        Ui.withIcon(scan, Glyph.SCAN);
        Ui.id(scan, "device.scan");
        actions.addView(scan, new LinearLayout.LayoutParams(0, Theme.dp(48), 1));
        pair = Ui.button(a, S.CONNECT, Ui.PRIMARY, a::pairDevice);
        Ui.id(pair, "device.connect");
        LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(0, Theme.dp(48), 1);
        cp.setMarginStart(Theme.dp(10));
        actions.addView(pair, cp);
        browse = Ui.button(a, S.BROWSE_FILES, Ui.SECONDARY, () -> shell.select(AppShell.FILES));
        Ui.id(browse, "device.browse");
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(0, Theme.dp(48), 1);
        bp.setMarginStart(Theme.dp(10));
        actions.addView(browse, bp);
        browse.setVisibility(View.GONE);
        // Ends every channel but keeps the saved pairing; “连接电脑” resumes.
        disconnect = Ui.button(a, S.DISCONNECT, Ui.DANGER, () -> a.disconnect(false));
        Ui.id(disconnect, "device.disconnect");
        LinearLayout.LayoutParams xp = new LinearLayout.LayoutParams(0, Theme.dp(48), 1);
        xp.setMarginStart(Theme.dp(10));
        actions.addView(disconnect, xp);
        disconnect.setVisibility(View.GONE);
        hero.addView(actions, Ui.lp(-1, -2));

        // Pairing details; collapses to a one-line summary once saved.
        formHeader = Ui.row(a);
        formHeader.setPadding(Theme.dp(4), Theme.dp(22), Theme.dp(4), Theme.dp(8));
        LinearLayout headerText = Ui.column(a);
        headerText.addView(Ui.medium(a, S.PAIRING, 15, Theme.INK));
        formSummary = Ui.text(a, "", 13, Theme.MUTED);
        Ui.oneLine(formSummary);
        headerText.addView(formSummary);
        formHeader.addView(headerText, Ui.weight(1));
        formChevron = new ImageView(a);
        formChevron.setImageDrawable(new Glyph(Glyph.CHEVRON_DOWN, Theme.MUTED));
        formHeader.addView(formChevron, Ui.lp(Theme.dp(24), Theme.dp(24)));
        formHeader.setBackground(Ui.press(Ui.round(0, 14), 14));
        formHeader.setOnClickListener(v -> setFormOpen(!formOpen));
        formHeader.setFocusable(true);
        Ui.role(formHeader, Button.class);
        Ui.id(formHeader, "device.pairing");
        content.addView(formHeader, Ui.lp(-1, -2));

        form = Ui.column(a);
        form.setBackground(Ui.outline(Theme.SURFACE, Theme.LINE, 20));
        form.setPadding(Theme.dp(16), Theme.dp(4), Theme.dp(16), Theme.dp(16));
        content.addView(form, Ui.lp(-1, -2));

        LinearLayout quick = Ui.row(a);
        quick.setPadding(0, Theme.dp(12), 0, Theme.dp(2));
        Button formScan = Ui.button(a, S.SCAN_PAIR, Ui.SECONDARY, a::scanPairing);
        Ui.withIcon(formScan, Glyph.SCAN);
        Ui.id(formScan, "device.form.scan");
        quick.addView(formScan, new LinearLayout.LayoutParams(0, Theme.dp(44), 1));
        Button pasteLink = Ui.button(a, S.PASTE_PAIR_LINK, Ui.SECONDARY, a::pastePairingLink);
        Ui.withIcon(pasteLink, Glyph.CLIPBOARD);
        Ui.id(pasteLink, "device.form.paste");
        LinearLayout.LayoutParams qp = new LinearLayout.LayoutParams(0, Theme.dp(44), 1);
        qp.setMarginStart(Theme.dp(8));
        quick.addView(pasteLink, qp);
        form.addView(quick, Ui.lp(-1, -2));

        form.addView(Ui.label(a, S.FIELD_HOST));
        LinearLayout hostRow = Ui.row(a);
        host = Ui.field(a, S.HINT_HOST, false);
        host.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        host.setContentDescription(S.FIELD_HOST);
        Ui.id(host, "device.host");
        hostRow.addView(host, Ui.weight(1));
        Button discover = Ui.button(a, S.DISCOVER, Ui.SECONDARY, a::discoverDevices);
        Ui.withIcon(discover, Glyph.RADAR);
        discover.setPadding(Theme.dp(14), 0, Theme.dp(16), 0);
        Ui.id(discover, "device.discover");
        LinearLayout.LayoutParams dp = Ui.lp(-2, Theme.dp(48));
        dp.setMarginStart(Theme.dp(8));
        hostRow.addView(discover, dp);
        form.addView(hostRow, Ui.lp(-1, -2));

        form.addView(Ui.label(a, S.FIELD_FINGERPRINT));
        LinearLayout pinRow = Ui.row(a);
        pinRow.setGravity(Gravity.TOP);
        pin = Ui.field(a, S.HINT_FINGERPRINT, false);
        pin.setTypeface(Theme.MONO);
        pin.setTextSize(13);
        pin.setSingleLine(false);
        pin.setMaxLines(3);
        pin.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        pin.setContentDescription(S.FIELD_FINGERPRINT);
        Ui.id(pin, "device.fingerprint");
        pinRow.addView(pin, Ui.weight(1));
        ImageView paste = Ui.iconButton(a, Glyph.PASTE, S.PASTE_FINGERPRINT, this::pasteFingerprint);
        paste.setBackground(Ui.press(Ui.round(Theme.INSET, 14), 14));
        LinearLayout.LayoutParams pp = Ui.lp(Theme.dp(48), Theme.dp(48));
        pp.setMarginStart(Theme.dp(8));
        pinRow.addView(paste, pp);
        form.addView(pinRow, Ui.lp(-1, -2));
        pinHint = Ui.text(a, S.HELP_FINGERPRINT, 12, Theme.MUTED);
        pinHint.setPadding(Theme.dp(2), Theme.dp(6), 0, 0);
        form.addView(pinHint);
        pin.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence c, int a1, int b, int d) {}
            public void onTextChanged(CharSequence c, int a1, int b, int d) {}
            public void afterTextChanged(Editable e) { validateFingerprint(); summarize(); }
        });

        form.addView(Ui.label(a, S.FIELD_SECRET));
        LinearLayout secretRow = Ui.row(a);
        secret = Ui.field(a, S.HINT_SECRET, true);
        secret.setTypeface(Theme.REGULAR);
        secret.setContentDescription(S.FIELD_SECRET);
        Ui.id(secret, "device.secret");
        secretRow.addView(secret, Ui.weight(1));
        ImageView[] eye = {null};
        eye[0] = Ui.iconButton(a, Glyph.EYE, S.SHOW_SECRET, () -> {
            secretVisible = !secretVisible;
            int selection = secret.getSelectionEnd();
            secret.setInputType(InputType.TYPE_CLASS_TEXT | (secretVisible ? InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD : InputType.TYPE_TEXT_VARIATION_PASSWORD));
            secret.setTypeface(Theme.REGULAR);
            if (selection >= 0) secret.setSelection(Math.min(selection, secret.length()));
            eye[0].setImageDrawable(new Glyph(secretVisible ? Glyph.EYE_OFF : Glyph.EYE, Theme.INK));
            eye[0].setContentDescription(secretVisible ? S.HIDE_SECRET : S.SHOW_SECRET);
        });
        eye[0].setBackground(Ui.press(Ui.round(Theme.INSET, 14), 14));
        LinearLayout.LayoutParams ep = Ui.lp(Theme.dp(48), Theme.dp(48));
        ep.setMarginStart(Theme.dp(8));
        secretRow.addView(eye[0], ep);
        form.addView(secretRow, Ui.lp(-1, -2));
        TextView secretHint = Ui.text(a, S.HELP_SECRET, 12, Theme.MUTED);
        secretHint.setPadding(Theme.dp(2), Theme.dp(6), 0, 0);
        form.addView(secretHint);
        TextWatcher summary = new TextWatcher() {
            public void beforeTextChanged(CharSequence c, int a1, int b, int d) {}
            public void onTextChanged(CharSequence c, int a1, int b, int d) {}
            public void afterTextChanged(Editable e) { summarize(); }
        };
        host.addTextChangedListener(summary);
        secret.addTextChangedListener(summary);

        // Tools.
        content.addView(Ui.heading(a, S.TOOLS));
        LinearLayout tools = Ui.column(a);
        tools.setBackground(Ui.outline(Theme.SURFACE, Theme.LINE, 20));
        tools.setPadding(Theme.dp(6), Theme.dp(6), Theme.dp(6), Theme.dp(6));
        content.addView(tools, Ui.lp(-1, -2));
        toolRow(tools, Glyph.PULSE, S.DIAGNOSTICS, S.DIAGNOSTICS_DETAIL, a::networkDiagnostics, "device.diagnostics", null);
        notifyState = Ui.text(a, "", 13, Theme.MUTED);
        toolRow(tools, Glyph.BELL, S.NOTIFICATIONS, S.NOTIFICATIONS_DETAIL, a::enableTransferNotifications, "device.notifications", notifyState);
        toolRow(tools, Glyph.LICENSE, S.LICENSES, S.LICENSES_DETAIL, a::showLicenses, "device.licenses", null);

        TextView footer = Ui.text(a, S.PRIVACY_NOTE, 12, Theme.FAINT);
        footer.setLineSpacing(0, 1.2f);
        footer.setPadding(Theme.dp(4), Theme.dp(18), Theme.dp(4), 0);
        content.addView(footer);
        summarize();
    }

    void toolRow(LinearLayout parent, int glyph, String title, String sub, Runnable action, String id, TextView trailing) {
        LinearLayout r = Ui.row(a);
        r.setPadding(Theme.dp(10), Theme.dp(10), Theme.dp(10), Theme.dp(10));
        r.setMinimumHeight(Theme.dp(60));
        r.addView(Ui.glyphTile(a, glyph, Theme.INK, Theme.INSET, 40, 12), Ui.lp(Theme.dp(40), Theme.dp(40)));
        LinearLayout lines = Ui.column(a);
        LinearLayout.LayoutParams p = Ui.weight(1);
        p.setMarginStart(Theme.dp(12));
        lines.addView(Ui.text(a, title, 16, Theme.INK));
        TextView d = Ui.text(a, sub, 13, Theme.MUTED);
        lines.addView(d);
        r.addView(lines, p);
        if (trailing != null) {
            LinearLayout.LayoutParams tp = Ui.lp(-2, -2);
            tp.setMarginStart(Theme.dp(8));
            r.addView(trailing, tp);
        }
        ImageView chevron = new ImageView(a);
        chevron.setImageDrawable(new Glyph(Glyph.CHEVRON_RIGHT, Theme.FAINT, Theme.dp(18)));
        r.addView(chevron, Ui.lp(Theme.dp(18), Theme.dp(18)));
        r.setBackground(Ui.press(Ui.round(0, 14), 14));
        r.setOnClickListener(v -> action.run());
        r.setFocusable(true);
        Ui.role(r, Button.class);
        Ui.id(r, id);
        parent.addView(r, Ui.lp(-1, -2));
    }

    void setFormOpen(boolean open) {
        formOpen = open;
        form.setVisibility(open ? View.VISIBLE : View.GONE);
        formChevron.setRotation(open ? 180 : 0);
        formSummary.setVisibility(open ? View.GONE : View.VISIBLE);
        formHeader.setContentDescription(S.PAIRING + "，" + (open ? S.EXPANDED : S.COLLAPSED));
    }

    void summarize() {
        boolean hasHost = host.length() > 0, hasPin = pin.length() > 0, hasSecret = secret.length() > 0;
        if (hasHost && hasPin && hasSecret) formSummary.setText(String.format(Locale.ROOT, S.PAIRING_SAVED, host.getText().toString().trim()));
        else formSummary.setText(S.PAIRING_INCOMPLETE);
        String fp = MainActivity.normalizeFingerprint(pin.getText().toString());
        fingerprintSummary.setText(fp.matches("[0-9a-f]{64}") ? String.format(Locale.ROOT, S.FINGERPRINT_SHORT, Ui.shortHex(fp)) : "");
        fingerprintSummary.setVisibility(fingerprintSummary.length() > 0 ? View.VISIBLE : View.GONE);
    }

    void validateFingerprint() {
        String raw = pin.getText().toString();
        String fp = MainActivity.normalizeFingerprint(raw);
        if (raw.trim().isEmpty() || fp.matches("[0-9a-f]{64}")) {
            pinHint.setText(S.HELP_FINGERPRINT);
            pinHint.setTextColor(Theme.MUTED);
        } else {
            pinHint.setText(String.format(Locale.ROOT, S.FINGERPRINT_LENGTH, fp.length()));
            pinHint.setTextColor(Theme.DANGER);
        }
    }

    void pasteFingerprint() {
        ClipboardManager clipboard = (ClipboardManager)a.getSystemService(Context.CLIPBOARD_SERVICE);
        ClipData clip = clipboard.getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0 || clip.getItemAt(0).getText() == null) {
            a.status(S.CLIPBOARD_EMPTY);
            return;
        }
        String fp = MainActivity.normalizeFingerprint(clip.getItemAt(0).getText().toString());
        if (!fp.matches("[0-9a-f]{64}")) {
            a.status(S.CLIPBOARD_NOT_FINGERPRINT);
            return;
        }
        pin.setText(fp);
    }

    void setConnection(int state, String address) {
        String where = address == null ? "" : address;
        // Connected or (re)connecting: offer disconnect (it also cancels a pending connect).
        boolean live = state == AppShell.CONNECTED || (state == AppShell.CONNECTING && a.credentials != null);
        pair.setVisibility(state == AppShell.CONNECTED ? View.GONE : View.VISIBLE);
        // Scanning leads only while nothing usable is saved; afterwards “连接电脑” is the main action.
        boolean offerScan = state != AppShell.CONNECTED && state != AppShell.CONNECTING && state != AppShell.SAVED;
        scan.setVisibility(offerScan ? View.VISIBLE : View.GONE);
        disconnect.setVisibility(live ? View.VISIBLE : View.GONE);
        switch (state) {
        case AppShell.CONNECTED:
            lamp.set(Lamp.ON);
            headline.setText(where.isEmpty() ? S.HERO_CONNECTED : String.format(Locale.ROOT, S.HERO_CONNECTED_TO, where));
            detail.setText(S.HERO_CONNECTED_DETAIL);
            browse.setVisibility(View.VISIBLE);
            Ui.style(browse, Ui.PRIMARY);
            setFormOpen(false);
            break;
        case AppShell.CONNECTING:
            lamp.set(Lamp.BUSY);
            headline.setText(where.isEmpty() ? S.HERO_CONNECTING : String.format(Locale.ROOT, S.HERO_CONNECTING_TO, where));
            detail.setText(S.HERO_CONNECTING_DETAIL);
            pair.setText(S.CONNECTING);
            break;
        case AppShell.FAILED:
            lamp.set(Lamp.ERROR);
            headline.setText(S.HERO_FAILED);
            detail.setText(S.HERO_FAILED_DETAIL);
            pair.setText(S.RETRY);
            Ui.style(pair, Ui.PRIMARY);
            browse.setVisibility(View.GONE);
            setFormOpen(true);
            break;
        case AppShell.INVALID:
            lamp.set(Lamp.ERROR);
            headline.setText(S.HERO_INVALID);
            detail.setText(S.HERO_INVALID_DETAIL);
            pair.setText(S.CONNECT);
            Ui.style(pair, Ui.PRIMARY);
            browse.setVisibility(View.GONE);
            setFormOpen(true);
            break;
        case AppShell.SAVED:
            lamp.set(Lamp.OFF);
            headline.setText(where.isEmpty() ? S.HERO_SAVED : String.format(Locale.ROOT, S.HERO_SAVED_AT, where));
            detail.setText(S.HERO_SAVED_DETAIL);
            pair.setText(S.CONNECT);
            Ui.style(pair, Ui.PRIMARY);
            browse.setVisibility(View.GONE);
            setFormOpen(false);
            break;
        default:
            lamp.set(Lamp.OFF);
            headline.setText(S.HERO_UNPAIRED);
            detail.setText(S.HERO_UNPAIRED_DETAIL);
            pair.setText(S.CONNECT);
            Ui.style(pair, Ui.PRIMARY);
            browse.setVisibility(View.GONE);
            setFormOpen(true);
        }
        if (offerScan)
            Ui.style(pair, Ui.SECONDARY);
    }

    void notificationsAllowed(boolean allowed) {
        notifyState.setText(allowed ? S.ALLOWED : S.NOT_ALLOWED);
        notifyState.setTextColor(allowed ? Theme.POSITIVE : Theme.MUTED);
    }
}
