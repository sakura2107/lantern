package org.lantern.client;
import android.content.Context;
import android.content.SharedPreferences;
import java.util.*;
import org.json.*;

/**
 * Computers this phone has connected to, one per certificate fingerprint. Each keeps its name, the
 * typed address, the learnt connection entries, the pairing secret (encrypted with the app's
 * Android Keystore key, never in plain text) and the entry that worked last time, so the next
 * connection tries that entry first instead of waiting for an unreachable LAN address.
 */
final class Computers {
    static final class Saved {
        final String pin, name, host, endpoints, iv, cipher, lastHost;
        final int port, lastPort;
        final long lastUsed;
        Saved(JSONObject o) throws JSONException {
            pin = o.getString("pin"); name = o.optString("name", ""); host = o.getString("host"); port = o.getInt("port");
            endpoints = o.optString("endpoints", ""); iv = o.getString("iv"); cipher = o.getString("cipher");
            lastHost = o.optString("lastHost", ""); lastPort = o.optInt("lastPort", 0); lastUsed = o.optLong("lastUsed", 0);
        }
        /** Name to show: the computer's own name from a pairing code, else its address. */
        String title() { return !name.isEmpty() ? name : address(); }
        String address() { return (host.indexOf(':') >= 0 ? "[" + host + "]" : host) + (port == 47443 ? "" : ":" + port); }
        String secret(Context context) throws Exception { return Secrets.decrypt(iv, cipher); }
    }
    static final int MAX = 16;
    static SharedPreferences prefs(Context c) { return c.getSharedPreferences("computers", 0); }

    static synchronized List<Saved> list(Context context) {
        List<Saved> out = new ArrayList<>();
        try {
            JSONArray array = new JSONArray(prefs(context).getString("list", "[]"));
            for (int i = 0; i < array.length(); i++)
                try { out.add(new Saved(array.getJSONObject(i))); } catch (JSONException skip) {}
        } catch (JSONException corrupt) {}
        out.sort((a, b) -> Long.compare(b.lastUsed, a.lastUsed));
        return out;
    }
    static Saved find(Context context, String pin) {
        for (Saved s : list(context)) if (s.pin.equals(pin)) return s;
        return null;
    }
    static synchronized void write(Context context, JSONArray array) {
        prefs(context).edit().putString("list", array.toString()).apply();
    }
    static synchronized JSONArray raw(Context context) {
        try { return new JSONArray(prefs(context).getString("list", "[]")); } catch (JSONException corrupt) { return new JSONArray(); }
    }
    /** After a verified connection: adds or refreshes this computer and makes it the most recent. */
    static synchronized void remember(Context context, Remote.Credentials c, ConnectionEndpoint via) throws Exception {
        JSONArray array = raw(context), next = new JSONArray();
        JSONObject entry = null;
        for (int i = 0; i < array.length(); i++) {
            JSONObject o = array.optJSONObject(i);
            if (o == null) continue;
            if (c.pin.equals(o.optString("pin"))) entry = o; else next.put(o);
        }
        if (entry == null) entry = new JSONObject().put("pin", c.pin);
        String[] sealed = Secrets.encrypt(c.token);
        entry.put("host", c.host).put("port", c.port).put("iv", sealed[0]).put("cipher", sealed[1])
             .put("lastUsed", System.currentTimeMillis());
        String pending = prefs(context).getString("name:" + c.pin, null);
        if (pending != null) entry.put("name", pending);
        if (!c.endpoints.isEmpty()) entry.put("endpoints", c.saveEndpointPreferences());
        if (via != null) entry.put("lastHost", via.host).put("lastPort", via.port);
        // Most recent first; the oldest beyond MAX are forgotten.
        JSONArray out = new JSONArray().put(entry);
        for (int i = 0; i < next.length() && out.length() < MAX; i++) out.put(next.get(i));
        write(context, out);
        prefs(context).edit().remove("name:" + c.pin).apply();
    }
    /** The entry the live connection uses now (presence checks), so the next start tries it first. */
    static synchronized void lastEntry(Context context, String pin, ConnectionEndpoint via) {
        if (via == null) return;
        JSONArray array = raw(context);
        for (int i = 0; i < array.length(); i++) {
            JSONObject o = array.optJSONObject(i);
            if (o != null && pin.equals(o.optString("pin")))
                try {
                    if (via.host.equals(o.optString("lastHost")) && via.port == o.optInt("lastPort")) return;
                    o.put("lastHost", via.host).put("lastPort", via.port); write(context, array); return;
                } catch (JSONException ignored) { return; }
        }
    }
    /** A pairing code carries the computer's name; kept until the first verified connection. */
    static void pendingName(Context context, String pin, String name) {
        if (name != null && !name.isEmpty()) prefs(context).edit().putString("name:" + pin, name).apply();
    }
    static synchronized void remove(Context context, String pin) {
        JSONArray array = raw(context), next = new JSONArray();
        for (int i = 0; i < array.length(); i++) {
            JSONObject o = array.optJSONObject(i);
            if (o != null && !pin.equals(o.optString("pin"))) next.put(o);
        }
        write(context, next);
    }
    /** Seeds the dialer's "worked last time" memory for every saved computer (process start). */
    static void seedDialer(Context context) {
        for (Saved s : list(context))
            if (!s.lastHost.isEmpty() && s.lastPort > 0) NetworkDialer.WINNERS.putIfAbsent(s.pin, s.lastHost + "\n" + s.lastPort);
    }
    /** v0.2 kept one computer in "device"/"secret" preferences: carried over once. */
    static void migrate(Context context) {
        if (prefs(context).getBoolean("migrated", false)) return;
        try {
            SharedPreferences device = context.getSharedPreferences("device", 0);
            String host = device.getString("host", ""), pin = device.getString("pin", "");
            String token = Secrets.load(context);
            if (!host.isEmpty() && pin.matches("[0-9a-f]{64}") && !token.isEmpty() && find(context, pin) == null) {
                List<ConnectionEndpoint> learnt = pin.equals(device.getString(MainActivity.ENDPOINTS_PIN, ""))
                    ? ConnectionEndpoint.loadPreferences(device.getString(MainActivity.ENDPOINTS_KEY, null)) : Collections.emptyList();
                Remote.Credentials c = new Remote.Credentials(host, device.getInt("port", 47443), pin, token, learnt);
                remember(context, c, null);
            }
            prefs(context).edit().putBoolean("migrated", true).apply();
        } catch (Exception keepTrying) { /* Keystore unavailable now: try again next start */ }
    }
}
