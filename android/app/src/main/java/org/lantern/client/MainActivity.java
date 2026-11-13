package org.lantern.client;
import android.app.*;
import android.content.*;
import android.graphics.*;
import android.net.Uri;
import android.os.*;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.security.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public final class MainActivity extends Activity {
    AppShell ui;
    NetworkWatcher networkWatcher;
    EditText host, pin, secret, path;
    TextView status;
    SystemAudio systemAudio;
    boolean audioSubscribed;
    static final int AUDIO_OFF = 0, AUDIO_CONNECTING = 1, AUDIO_ON = 2, AUDIO_RECONNECTING = 3;
    int audioState = AUDIO_OFF;
    RemoteKeyboard keyboard;
    volatile int lastFocusFlags;
    final AtomicLong keyboardRequestId = new AtomicLong();
    final Handler inputUi = new Handler(Looper.getMainLooper());
    boolean draggingRemote, rightArmed;
    float dragX, dragY;
    final AtomicLong dragEndsCompleted = new AtomicLong(), textCommitsCompleted = new AtomicLong();
    final Runnable dragHeartbeat = new Runnable() {
        public void run() {
            if (!draggingRemote) return;
            desktopDragMove(dragX, dragY); inputUi.postDelayed(this, 250);
        }
    };
    volatile boolean controlRequested;
    final AtomicLong inputEpoch = new AtomicLong(), controlClicksCompleted = new AtomicLong();
    FileBrowser browser;
    TransferQueue transfers;
    ListView list;
    DesktopView screen;
    DesktopLayout desktopLayout;
    DesktopTools desktopTools;volatile int selectedMonitor=0;final AtomicLong monitorGeneration=new AtomicLong();
    AvcView avcView;
    AvcDesktop video;
    volatile boolean hardwareDesktop = true;
    volatile AvcDesktop.Profile videoProfile;
    final ThreadPoolExecutor work =
        new ThreadPoolExecutor(1, 1, 0, TimeUnit.SECONDS, new ArrayBlockingQueue<>(16));
    final ScheduledExecutorService frames = Executors.newSingleThreadScheduledExecutor();
    final AtomicBoolean cancelled = new AtomicBoolean(), subscribed = new AtomicBoolean(),
                        display = new AtomicBoolean(true), control = new AtomicBoolean();
    volatile Remote.Credentials credentials;
    volatile Remote desktop;
    static final class DesktopProfile {
        final int width, quality;
        DesktopProfile(int w, int q) {
            width = w;
            quality = q;
        }
    }
    static final DesktopProfile[] PROFILES = {new DesktopProfile(960, 55), new DesktopProfile(1280, 65),
                                              new DesktopProfile(1920, 75)};
    volatile DesktopProfile desktopProfile = PROFILES[1];
    Remote.Credentials desktopCredentials;
    int offset = 0;
    List<Remote.Entry> entries = new ArrayList<>();
    final AtomicReference<Bitmap> latestBitmap = new AtomicReference<>();
    final AtomicBoolean imagePosted = new AtomicBoolean();
    final AtomicReference<String> latestStatus = new AtomicReference<>();
    final AtomicBoolean statusPosted = new AtomicBoolean();
    interface Job {
        void run() throws Exception;
    }
    void job(Job j) {
        try {
            work.execute(() -> {
                try {
                    j.run();
                } catch (Exception e) {
                    status(e.getMessage());
                }
            });
        } catch (RejectedExecutionException e) {
            status("操作队列已满");
        }
    }
    void status(String text) {
        // Any channel (files, picture, sound, control) can be the one that learns the host ended us.
        if (text != null && text.contains(Remote.ENDED_BY_HOST) && credentials != null)
            runOnUiThread(() -> disconnect(true));
        latestStatus.set(text == null ? "操作失败" : text);
        if (statusPosted.compareAndSet(false, true))
            runOnUiThread(() -> {
                String latest = latestStatus.getAndSet(null);
                statusPosted.set(false);
                if (latest != null && ui != null)
                    ui.notice(latest);
                if (latest != null && desktopLayout != null)
                    desktopLayout.notice(latest);
            });
    }
    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        ui=new AppShell(this);setContentView(ui.window);
        getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Theme.BG));
        host=ui.host;pin=ui.pin;secret=ui.secret;path=ui.path;status=ui.status;list=ui.fileList;
        var preferences = getSharedPreferences("device", 0);
        desktopProfile =
            PROFILES[Math.max(0, Math.min(PROFILES.length - 1, preferences.getInt("desktop_profile", 1)))];
        hardwareDesktop = preferences.getBoolean("desktop_h264", true);
        if (preferences.contains("video_width")) {
            qualityWidth = preferences.getInt("video_width", 1920); qualityFps = preferences.getInt("video_fps", 60); qualityAuto = preferences.getBoolean("video_auto", false);
        } else { // v0.2 presets: 0 = 1080p60, 1 = 720p60, 2 = 720p30
            int old = preferences.getInt("video_profile", 0);
            qualityWidth = old == 0 ? 1920 : 1280; qualityFps = old == 2 ? 30 : 60;
        }
        if (Arrays.binarySearch(new int[]{1280, 1920, 2560, 3840}, qualityWidth) < 0) qualityWidth = 1920;
        if (qualityFps != 30 && qualityFps != 60 && qualityFps != 120) qualityFps = 60;
        videoProfile = qualityProfile();
        long limit = preferences.getLong("transfer_limit", 0);
        if (limit >= 1024 * 1024 && limit <= 32L * 1024 * 1024) TransferPacer.SHARED.userLimit = limit;
        String savedHost=preferences.getString("host", "");host.setText(savedHost.isEmpty()?"":(savedHost.indexOf(':')>=0?"["+savedHost+"]":savedHost)+":"+preferences.getInt("port",47443));
        pin.setText(preferences.getString("pin", ""));
        if (!savedHost.isEmpty()) ui.setConnection(AppShell.SAVED, host.getText().toString());
        job(() -> {
            String value = Secrets.load(this);
            runOnUiThread(() -> { secret.setText(value); ui.device.summarize(); });
            Computers.migrate(this);
            Computers.seedDialer(this);
            refreshComputers();
        });
        desktopTools=new DesktopTools(this,new DesktopTools.Events(){public void status(String value){MainActivity.this.status(value);}public void monitors(List<DesktopTools.Monitor> values){showMonitors(values);}});
        screen = new DesktopView(this);
        screen.setBackgroundColor(Color.TRANSPARENT);
        screen.setVisibility(View.GONE);
        desktopLayout = new DesktopLayout(this, ui.window, ui.root, ui.desktop.preview, screen, new DesktopLayout.Actions() {
            public void quality() { showQuality(); }
            public void control() { toggleControl(); }
            public void keyboard() { showKeyboard(); }
            public void audio() { toggleAudio(); }
            public void files() { desktopLayout.setFullscreen(false); ui.select(AppShell.FILES); }
            public void tools() { showDesktopTools(); }
            public void rightClick() { toggleRightClick(); }
        });
        desktopLayout.systemBars();
        systemAudio = new SystemAudio(this, new SystemAudio.Events() {
            public void started(int rate) { if(audioSubscribed) { boolean again=audioSeen; audioSeen=true; audioLink=null; audioState(AUDIO_ON); if(!again) status("系统音频已连接："+rate+"Hz / "+systemAudio.encoding); } }
            public void failed(String reason) { audioSubscribed=false; audioState(AUDIO_OFF); status("系统音频停止："+reason); }
            // Recovery is shown by the link banner; the sound toggle stays on.
            public void reconnecting(String reason){if(audioSubscribed)audioState(AUDIO_RECONNECTING);}
            public void connectionState(ConnectionStatus.Snapshot state){if(!audioSubscribed)return;audioLink=state;audioLinkAt=SystemClock.uptimeMillis();if(state.phase==ConnectionStatus.Phase.NORMAL&&audioState==AUDIO_RECONNECTING)audioState(AUDIO_ON);linkChanged();}
        });
        keyboard = new RemoteKeyboard(this, new RemoteKeyboard.Sink() {
            public void text(String text) { desktopText(text); }
            public void key(int vk, int count) { desktopKey(vk, count); }
        });
        desktopLayout.pane.addView(keyboard, new FrameLayout.LayoutParams(1, 1));
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        avcView = new AvcView(this);
        avcView.setVisibility(View.GONE);
        desktopLayout.pane.addView(avcView, 0, new FrameLayout.LayoutParams(-1, -1));
        screen.viewportChanged = ()
            -> avcView.viewport(screen.viewport);
        video = new AvcDesktop(avcView, new AvcDesktop.Events() {
            public void configured(int w, int h, String decoder, boolean hardware) {
                if (!hardwareDesktop || !subscribed.get())
                    return;
                screen.videoSize(w, h);
                String codec = decoder.toLowerCase(Locale.ROOT).contains("hevc") ? "HEVC" : "H264";
                screen.codecLabel = codec;
                status(codec + " 桌面：" + w + "×" + h + "，" + (hardware ? "硬件解码" : "解码器硬件属性未知"));
            }
            public void frame() {
                if (!videoSeen && hardwareDesktop && subscribed.get()) { videoSeen = true; videoLink = null; loadQualityLimits(credentials); linkChanged(); }
                if (hardwareDesktop && subscribed.get() && display.get())
                    screen.videoFrame();
            }
            public void syncWarning(){status("音频输出延迟超过同步预算；画面优先及时显示，当前音画可能有偏差");}
            public void failed(String reason,boolean fallback) {
                if(!fallback){subscribed.set(false);updateDesktopPresentation();status("桌面连接停止，请检查网络或授权："+reason);return;}
                hardwareDesktop = false;
                updateDesktopPresentation();
                status("硬件桌面不可用，已降级为 JPEG：" + reason);
            }
            // Only the picture reconnects; control, keyboard and sound keep their own connections.
            // The picture recovers on its own; control and sound keep running. The banner shows progress.
            public void reconnecting(String reason){}
            public void connectionState(ConnectionStatus.Snapshot state){videoLink=state;videoLinkAt=SystemClock.uptimeMillis();linkChanged();}
            public void quality(DesktopSession.Status status){if(hardwareDesktop&&subscribed.get())MainActivity.this.quality(status);}
        });
        screen.tap = this::desktopTap;
        screen.doubleTap = (x, y) -> desktopClick(x, y, true);
        screen.twoFingerTap = this::desktopRightClick;
        screen.drag = new DesktopView.Drag() {
            public boolean begin(float x, float y) { return desktopDragBegin(x, y); }
            public void move(float x, float y) { desktopDragMove(x, y); }
            public void end(float x, float y, boolean cancel) { desktopDragEnd(x, y, cancel); }
        };
        browser = new FileBrowser(this);
        transfers = TransferService.acquire(this);
        systemAudio.timelineSource=()->timeline;video.timelineSource=()->timeline;
        if(transfers.credentials!=null){credentials=transfers.credentials;ui.setConnection(AppShell.SAVED,display(credentials));}
        if(getIntent().getBooleanExtra("open_transfers",false))ui.select(AppShell.TRANSFERS);
        ui.device.notificationsAllowed(notificationsAllowed());
        if(state==null)handleViewIntent(getIntent());
        refreshDesktopUi();
        Runnable frameTask = new Runnable() {
            public void run() {
                long began = System.nanoTime();final long monitorEpoch=monitorGeneration.get();final int captureMonitor=selectedMonitor;
                try {
                    if (!subscribed.get()) {
                        video.stop();
                        if (desktop != null) {
                            desktop.close();
                            desktop = null;
                        }
                        return;
                    }
                    Remote.Credentials c = credentials;
                    if (c == null)
                        throw new IOException("请先配对");
                    if (hardwareDesktop) {
                        if (desktop != null) {
                            desktop.close();
                            desktop = null;
                        }
                        video.start(c, videoProfile);
                        return;
                    }
                    video.stop();
                    if (desktop != null && desktopCredentials != c) {
                        desktop.close();
                        desktop = null;
                    }
                    if (desktop == null) {
                        desktop = new Remote(c, 4, 1);
                        desktopCredentials = c;
                    }
                    DesktopProfile profile = desktopProfile;
                    byte[] answer = desktop.call(21, Remote.body(o -> {
                        o.writeInt(captureMonitor);
                        o.writeInt(profile.width);
                        o.writeInt(profile.quality);
                    }));
                    byte[] jpg = Remote.blob(Remote.reader(answer), Remote.MAX);
                    if (display.get() && subscribed.get() && c == credentials && monitorEpoch==monitorGeneration.get()) {
                        Bitmap bitmap = BitmapFactory.decodeByteArray(jpg, 0, jpg.length);
                        if (bitmap == null)
                            throw new IOException("JPEG 解码失败");
                        Bitmap stale = latestBitmap.getAndSet(bitmap);
                        if (stale != null)
                            stale.recycle();
                        if (imagePosted.compareAndSet(false, true))
                            runOnUiThread(() -> {
                                Bitmap latest = latestBitmap.getAndSet(null);
                                imagePosted.set(false);
                                if (latest != null) {
                                    if (subscribed.get() && display.get() && c == credentials &&
                                        !isDestroyed() && monitorEpoch==monitorGeneration.get())
                                        screen.setImageBitmap(latest);
                                    else
                                        latest.recycle();
                                }
                            });
                    }
                } catch (Exception e) {
                    subscribed.set(false);
                    try {
                        if (desktop != null)
                            desktop.close();
                    } catch (Exception ignored) {
                    }
                    desktop = null;
                    status(e.getMessage());
                    runOnUiThread(() -> { if (!isDestroyed()) updateDesktopPresentation(); });
                } finally {
                    // One in-flight request and one latest bitmap. Missed deadlines are skipped;
                    // capture/network/decode time is part of the 33 ms cycle, not added to it.
                    if (!frames.isShutdown()) {
                        long wait = subscribed.get() ? Math.max(0, 33_333_333L - (System.nanoTime() - began))
                                                     : 100_000_000L;
                        try {
                            frames.schedule(this, wait, TimeUnit.NANOSECONDS);
                        } catch (RejectedExecutionException ignored) {
                        }
                    } else if (desktop != null) {
                        try {
                            desktop.close();
                        } catch (Exception ignored) {
                        }
                        desktop = null;
                    }
                }
            }
        };
        frames.execute(frameTask);
    }
    /** The typed address, plus every entry learnt for the computer with this fingerprint (never another one's). */
    Remote.Credentials credentialsForAddress(String address,String fingerprint,String key){
        Endpoint endpoint;
        try{endpoint=Endpoint.parse(address,47443);}catch(IOException error){throw new IllegalArgumentException(error.getMessage());}
        List<ConnectionEndpoint> learnt=savedEndpoints(fingerprint);
        if(learnt.isEmpty()||!ConnectionEndpoint.validHost(endpoint.host))
            return new Remote.Credentials(endpoint.host,endpoint.port,fingerprint,key);
        return new Remote.Credentials(endpoint.host,endpoint.port,fingerprint,key,learnt)
            .mergeEndpoints(java.util.Collections.singletonList(ConnectionEndpoint.of(endpoint.host,endpoint.port)));
    }
    // ---------------------------------------------------------------- connection entries
    static final String ENDPOINTS_KEY = "endpoints_v1", ENDPOINTS_PIN = "endpoints_pin";
    /** The entry the live connection uses (LAN, public IPv6, hostname…); null until verified. */
    volatile ConnectionEndpoint via;
    List<ConnectionEndpoint> savedEndpoints(String fingerprint){
        var p=getSharedPreferences("device",0);
        if(fingerprint.equals(p.getString(ENDPOINTS_PIN,"")))return ConnectionEndpoint.loadPreferences(p.getString(ENDPOINTS_KEY,null));
        Computers.Saved other=Computers.find(this,fingerprint); // another saved computer
        return other!=null&&!other.endpoints.isEmpty()?ConnectionEndpoint.loadPreferences(other.endpoints):java.util.Collections.emptyList();
    }
    void saveEndpoints(Remote.Credentials c){
        if(c.endpoints.isEmpty())return;
        getSharedPreferences("device",0).edit().putString(ENDPOINTS_KEY,c.saveEndpointPreferences()).putString(ENDPOINTS_PIN,c.pin).apply();
    }
    /** After a verified connection: count the entries that failed, ask the computer for its current ones. */
    Remote.Credentials learn(Remote.Credentials c,Remote remote){
        List<ConnectionEndpoint> base=c.endpoints;
        if(base.isEmpty()&&ConnectionEndpoint.validHost(c.host))base=java.util.Collections.singletonList(ConnectionEndpoint.of(c.host,c.port));
        List<ConnectionEndpoint> fresh;
        try{fresh=remote.endpoints();}catch(Exception older){fresh=java.util.Collections.emptyList();}
        return new Remote.Credentials(c.host,c.port,c.pin,c.token,ConnectionEndpoint.recordResults(base,remote.connectionAttempts)).mergeEndpoints(fresh);
    }
    /** IPv6 with the longest zero run compressed, for people; stored entries keep every group. */
    static String hostText(ConnectionEndpoint e){
        String h=e.host;
        if(h.indexOf(':')>=0){
            String[] g=h.split(":",-1);int best=-1,len=0;
            for(int i=0;i<g.length;){int j=i;while(j<g.length&&g[j].equals("0"))j++;if(j-i>len&&j-i>1){best=i;len=j-i;}i=j==i?i+1:j;}
            if(best>=0){StringBuilder s=new StringBuilder();for(int i=0;i<best;i++)s.append(i>0?":":"").append(g[i]);s.append("::");for(int i=best+len;i<g.length;i++)s.append(i>best+len?":":"").append(g[i]);h=s.toString();}
            h="["+h+"]";
        }
        return h+":"+e.port;
    }
    /** “局域网 · 192.168.1.5:47443” once verified; the saved address otherwise. */
    String where(Remote.Credentials c){
        ConnectionEndpoint e=via;
        if(e==null||e.kind<1||e.kind>5)return display(c);
        String text=hostText(e);
        return S.PATH_LABELS[e.kind]+" · "+(e.port==47443?text.substring(0,text.lastIndexOf(':')):text);
    }
    static String attemptsText(NetworkDialer.ConnectException error){
        StringBuilder s=new StringBuilder();
        for(NetworkDialer.Probe p:error.attempts){
            if(p.failure==NetworkDialer.Failure.CANCELLED)continue;
            if(s.length()>0)s.append("；");
            s.append(hostText(p.endpoint)).append(' ').append(p.failure==null?S.ENDPOINT_OK:S.ENDPOINT_FAILURES[p.failure.ordinal()]);
        }
        return String.format(Locale.ROOT,S.CONNECT_ALL_FAILED,s);
    }
    /** Every saved entry on its own and at the same time (an unreachable LAN address must not add 10 s each). */
    static List<NetworkDialer.Probe> probeEntries(Remote.Credentials c){
        List<ConnectionEndpoint> all=c.candidates();
        List<NetworkDialer.Probe> out=new ArrayList<>();
        if(all.size()<2)return out;
        java.util.concurrent.ExecutorService pool=java.util.concurrent.Executors.newFixedThreadPool(all.size());
        try{
            List<java.util.concurrent.Future<List<NetworkDialer.Probe>>> runs=new ArrayList<>();
            for(ConnectionEndpoint e:all)
                runs.add(pool.submit(()->Remote.probeEndpoints(new Remote.Credentials(e.host,e.port,c.pin,c.token,java.util.Collections.singletonList(e)))));
            for(int i=0;i<runs.size();i++)
                try{out.addAll(runs.get(i).get(15,java.util.concurrent.TimeUnit.SECONDS));}
                catch(Exception slow){out.add(new NetworkDialer.Probe(all.get(i),NetworkDialer.Failure.TIMEOUT,15000,0,false,false,null));}
        }finally{pool.shutdownNow();}
        return out;
    }
    static String probeText(NetworkDialer.Probe p){
        if(p.failure!=null)return S.ENDPOINT_FAILURES[p.failure.ordinal()];
        if(p.rttMs==null)return S.ENDPOINT_OK;
        return p.rttMs<1?S.ENDPOINT_RTT_FAST:String.format(Locale.ROOT,S.ENDPOINT_RTT,p.rttMs);
    }

    volatile MediaTimeline timeline=new MediaTimeline();
    final AtomicBoolean discoveryCancel=new AtomicBoolean();
    void discoverDevices(){
        discoveryCancel.set(false);status(S.DISCOVERING);
        job(()->{
            java.util.List<Discovery.Device> found=Discovery.scan(discoveryCancel,null);
            runOnUiThread(()->{
                if(isDestroyed()||isFinishing()||discoveryCancel.get())return;
                if(found.isEmpty()){Sheet.info(this,S.DISCOVER_NONE,S.DISCOVER_NONE_DETAIL);return;}
                Sheet sheet=new Sheet(this,S.DISCOVER_TITLE,S.DISCOVER_SUBTITLE);
                for(Discovery.Device d:found)sheet.item(Glyph.DESKTOP,d.label,d.address(),()->verifyDiscovered(d));
                sheet.show();
            });
        });
    }
    /** Discovery is unauthenticated: it fills the address only. The fingerprint is replaced solely by an explicit, checked choice. */
    void verifyDiscovered(Discovery.Device d){
        host.setText(d.address());
        String saved=normalizeFingerprint(pin.getText().toString());
        boolean differs=saved.matches("[0-9a-f]{64}")&&!saved.equals(d.pin);
        Sheet sheet=new Sheet(this,S.VERIFY_TITLE,differs?S.VERIFY_DIFFERS:S.VERIFY_DETAIL);
        TextView print=Ui.text(this,Ui.groupHex(d.pin),15,Theme.INK);
        print.setTypeface(Theme.MONO);print.setLineSpacing(0,1.25f);print.setTextIsSelectable(true);
        print.setBackground(Ui.round(differs?Theme.DANGER_SOFT:Theme.INSET,14));print.setPadding(Theme.dp(14),Theme.dp(12),Theme.dp(14),Theme.dp(12));
        LinearLayout box=Ui.column(this);box.setPadding(Theme.dp(16),0,Theme.dp(16),Theme.dp(8));box.addView(print,Ui.lp(-1,-2));
        sheet.view(box);
        sheet.buttons(S.ADDRESS_ONLY,S.USE_FINGERPRINT,differs?Ui.DANGER:Ui.PRIMARY,()->{pin.setText(d.pin);status(S.FINGERPRINT_FILLED);});
        sheet.show();
    }
    static String normalizeFingerprint(String raw){
        String compact=raw.replaceAll("[\\s:-]","").toLowerCase(Locale.ROOT);
        return compact.matches("[0-9a-f]{64}")?compact:raw.trim();
    }
    static String display(Remote.Credentials c){return (c.host.indexOf(':')>=0?"["+c.host+"]":c.host)+(c.port==47443?"":":"+c.port);}
    void pairDevice() {
        timeline=new MediaTimeline();
        if(desktopTools!=null)desktopTools.cancel();
        browser.cancel();entries=new ArrayList<>();ui.renderFiles(entries,false);ui.files.noReadPermission=false;
            audioSubscribed=false; if(systemAudio!=null) systemAudio.stop(); audioState(AUDIO_OFF);
            String fingerprint=normalizeFingerprint(pin.getText().toString());if(!fingerprint.equals(pin.getText().toString()))pin.setText(fingerprint);
            Remote.Credentials c;try{c=credentialsForAddress(host.getText().toString().trim(),fingerprint,secret.getText().toString().trim());}catch(IllegalArgumentException e){status(e.getMessage());ui.setConnection(AppShell.INVALID,"");return;}
            controlRequested = false; control.set(false); inputEpoch.incrementAndGet(); closeInput(); controlLabels();
            credentials = c;
            offset = 0;ui.setConnection(AppShell.CONNECTING,display(c));
            via = null;
            job(() -> {
                Remote.Credentials learnt;
                ConnectionEndpoint used;
                // Deliberate connection: clears a host-side "ended" hold for this device (session=end-v1).
                try (Remote remote = new Remote(c.asDeliberate(), 0, 0)) {
                    String caps = Remote.str(Remote.reader(remote.call(30, new byte[0])));
                    Remote.sessionEndPin = caps.contains("session=end-v1") ? c.pin : null;
                    learnt = learn(c, remote);
                    used = remote.selectedEndpoint;
                } catch(Exception error) {
                    String reason=error.getMessage();
                    if(error instanceof NetworkDialer.ConnectException){
                        NetworkDialer.ConnectException failed=(NetworkDialer.ConnectException)error;
                        if(failed.reason!=NetworkDialer.Failure.PIN_MISMATCH){
                            reason=attemptsText(failed);
                            // Same computer, entries that keep failing are dropped after a few tries.
                            if(!c.endpoints.isEmpty())saveEndpoints(new Remote.Credentials(c.host,c.port,c.pin,c.token,ConnectionEndpoint.recordResults(c.endpoints,failed.attempts)));
                        }
                    }
                    String why=reason;
                    runOnUiThread(()->{if(credentials!=c)return;ui.setConnection(AppShell.FAILED,display(c));if(why!=null&&!why.isEmpty())ui.deviceState.setText(String.format(Locale.ROOT,S.HERO_FAILED_REASON,why));});
                    throw error;
                }
                Secrets.save(this, c.token);
                getSharedPreferences("device", 0)
                    .edit()
                    .putString("host", c.host).putInt("port",c.port)
                    .putString("pin", c.pin)
                    .apply();
                saveEndpoints(learnt);
                try { Computers.remember(this, learnt, used); } catch (Exception keystore) { /* the list is a convenience */ }
                refreshComputers();
                // Later connections race every entry; identity checks below follow the new object.
                if (credentials == c) { credentials = learnt; via = used; }
                runOnUiThread(()->{if(credentials!=learnt)return;ui.connected(where(learnt));ui.select(AppShell.FILES);transfers.setCredentials(learnt);refreshDesktopUi();startPresence(learnt);if(used!=null&&used.publicPath())status(S.PATH_PUBLIC_NOTICE);});
                try { refresh(); }
                catch(Remote.RemoteError error) {
                    if(error.code!=2) throw error;
                    // Reauthenticate without file scope: distinguish denied file access from revoked trust.
                    try(Remote identity=new Remote(c,0,0)) {identity.call(30,new byte[0]);}
                    runOnUiThread(() -> { entries=new ArrayList<>(); ui.files.noReadPermission=true; ui.renderFiles(entries,false); status("已配对，未授予文件读取；可按授权使用桌面或音频"); });
                }
            });
    }
    // ---------------------------------------------------------------- saved computers
    void refreshComputers() {
        job(() -> {
            List<Computers.Saved> saved = Computers.list(this);
            runOnUiThread(() -> ui.device.computers(saved, credentials == null ? "" : credentials.pin));
        });
    }
    /** Lists every saved computer; tapping one connects to it with its own pairing secret. */
    void showComputers() {
        job(() -> {
            List<Computers.Saved> saved = Computers.list(this);
            runOnUiThread(() -> {
                String current = credentials == null ? pin.getText().toString() : credentials.pin;
                Sheet sheet = new Sheet(this, S.COMPUTERS_TITLE, S.COMPUTERS_SUBTITLE);
                for (Computers.Saved s : saved)
                    sheet.option(s.title(), computerDetail(s), s.pin.equals(current), () -> switchComputer(s));
                sheet.item(Glyph.PLUS, S.COMPUTERS_ADD, S.COMPUTERS_ADD_DETAIL, this::scanPairing);
                if (!saved.isEmpty()) sheet.item(Glyph.TRASH, S.COMPUTERS_REMOVE, S.COMPUTERS_REMOVE_DETAIL, () -> removeComputers(saved));
                sheet.show();
            });
        });
    }
    String computerDetail(Computers.Saved s) {
        String when = s.lastUsed <= 0 ? "" : "，" + String.format(Locale.ROOT, S.COMPUTERS_LAST_USED,
            java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.MEDIUM, java.text.DateFormat.SHORT).format(new java.util.Date(s.lastUsed)));
        return (s.name.isEmpty() ? "" : s.address() + "，") + S.COMPUTERS_PRINT + s.pin.substring(0, 8) + when;
    }
    void switchComputer(Computers.Saved s) {
        job(() -> {
            String key = s.secret(this);
            runOnUiThread(() -> {
                if (credentials != null && !credentials.pin.equals(s.pin)) disconnect(false);
                host.setText((s.host.indexOf(':') >= 0 ? "[" + s.host + "]" : s.host) + ":" + s.port);
                pin.setText(s.pin);
                secret.setText(key);
                ui.device.summarize();
                pairDevice();
            });
        });
    }
    void removeComputers(List<Computers.Saved> saved) {
        Sheet sheet = new Sheet(this, S.COMPUTERS_REMOVE, S.COMPUTERS_REMOVE_SUBTITLE);
        for (Computers.Saved s : saved)
            sheet.danger(Glyph.DESKTOP, s.title(), computerDetail(s), () ->
                Sheet.confirm(this, S.COMPUTERS_REMOVE, String.format(Locale.ROOT, S.COMPUTERS_REMOVE_CONFIRM, s.title()), S.COMPUTERS_REMOVE_ACTION, true, () -> {
                    job(() -> { Computers.remove(this, s.pin); refreshComputers(); status(String.format(Locale.ROOT, S.COMPUTERS_REMOVED, s.title())); });
                }));
        sheet.show();
    }
    // ---------------------------------------------------------------- scan-to-pair
    static final int REQUEST_SCAN = 103, REQUEST_CAMERA = 202;
    /** Opens the in-app scanner (asks for the camera first). */
    void scanPairing() {
        if (checkSelfPermission(android.Manifest.permission.CAMERA) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {android.Manifest.permission.CAMERA}, REQUEST_CAMERA);
            return;
        }
        startActivityForResult(new Intent(this, ScanActivity.class), REQUEST_SCAN);
    }
    /** Pairing link from the clipboard (copied from the computer's pairing window or sent by chat). */
    void pastePairingLink() {
        android.content.ClipboardManager clipboard = (android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE);
        android.content.ClipData clip = clipboard.getPrimaryClip();
        CharSequence text = clip == null || clip.getItemCount() == 0 ? null : clip.getItemAt(0).getText();
        if (text == null || !text.toString().trim().startsWith(ScanActivity.PREFIX)) {
            status(S.CLIPBOARD_NOT_LINK);
            return;
        }
        pairingLink(text.toString().trim());
    }
    /** lantern://pair links opened from a camera app, a browser or chat. */
    void handleViewIntent(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction()) || intent.getData() == null ||
            !"lantern".equals(intent.getData().getScheme()))
            return;
        String raw = intent.getData().toString();
        intent.setData(null); // a recreated activity must not pair again
        if (desktopLayout != null)
            desktopLayout.setFullscreen(false);
        ui.select(AppShell.DEVICE);
        pairingLink(raw);
    }
    /** Validates the link offline, then asks the user to confirm which computer this is. */
    void pairingLink(String raw) {
        PairingLink link = raw == null ? null : PairingLink.parse(raw);
        if (link == null) {
            Sheet.info(this, S.LINK_INVALID_TITLE, S.LINK_INVALID_DETAIL);
            return;
        }
        StringBuilder groups = new StringBuilder();
        for (int i = 0; i < 16; i += 4)
            groups.append(link.pin, i, i + 4).append(' ');
        String address = link.endpoints.isEmpty() ? link.addresses.get(0) + ":" + link.port
                         : link.endpoints.size() == 1 ? hostText(link.endpoints.get(0))
                         : String.format(Locale.ROOT, S.MORE_ADDRESSES, hostText(link.endpoints.get(0)), link.endpoints.size());
        Sheet.confirm(this, S.PAIR_CONFIRM_TITLE,
                      String.format(Locale.ROOT, S.PAIR_CONFIRM, link.hostName, address, groups.toString().trim()),
                      S.PAIR_CONFIRM_ACTION, false, () -> pairWith(link));
    }
    static String deviceLabel() {
        String maker = android.os.Build.MANUFACTURER == null ? "" : android.os.Build.MANUFACTURER.trim();
        String model = android.os.Build.MODEL == null ? "" : android.os.Build.MODEL.trim();
        if (!maker.isEmpty() && !model.toLowerCase(Locale.ROOT).startsWith(maker.toLowerCase(Locale.ROOT)))
            model = Character.toUpperCase(maker.charAt(0)) + maker.substring(1) + " " + model;
        return PairingLink.validLabel(model) ? model : "Android 手机";
    }
    /** Enrols over pinned TLS, then reuses the normal connect path (which saves and connects). */
    void pairWith(PairingLink link) {
        String label = deviceLabel();
        ui.setConnection(AppShell.CONNECTING, link.hostName);
        status(String.format(Locale.ROOT, S.PAIRING_NOW, link.hostName));
        job(() -> {
            Remote.Credentials c;
            try {
                c = Remote.pair(link, label);
            } catch (Remote.PairingException e) {
                String why = S.PAIR_ERRORS[e.reason.ordinal()];
                runOnUiThread(() -> {
                    ui.setConnection(AppShell.FAILED, link.hostName);
                    ui.deviceState.setText(why);
                    Sheet.info(this, S.PAIR_FAILED_TITLE, why);
                });
                return;
            }
            saveEndpoints(c); // every entry from the code; pairDevice() reads them back for this fingerprint
            Computers.pendingName(this, c.pin, link.hostName);
            runOnUiThread(() -> {
                host.setText((c.host.indexOf(':') >= 0 ? "[" + c.host + "]" : c.host) + ":" + c.port);
                pin.setText(c.pin);
                secret.setText(c.token);
                ui.device.summarize();
                status(S.PAIRED_OK);
                pairDevice();
            });
        });
    }
    /**
     * Ends every channel to the computer and keeps the saved pairing (address, fingerprint, key):
     * picture, sound, control, file browsing and transfers stop; “连接电脑” resumes.
     * byHost: the computer ended this device's sessions (session=end-v1), so we must not reconnect.
     */
    void disconnect(boolean byHost) {
        Remote.Credentials c = credentials;
        if (c == null) return;
        credentials = null;
        via = null;
        presenceSeq.incrementAndGet();
        closePresence();
        if (desktopLayout != null) desktopLayout.setFullscreen(false);
        if (subscribed.get()) { subscribed.set(false); updateDesktopPresentation(); }
        audioSubscribed = false; if (systemAudio != null) systemAudio.stop(); audioState(AUDIO_OFF);
        controlRequested = false; control.set(false); inputEpoch.incrementAndGet(); closeInput(); controlLabels();
        if (desktopTools != null) desktopTools.cancel();
        browser.cancel(); entries = new ArrayList<>(); ui.renderFiles(entries, false);
        transfers.setCredentials(null); // pauses the running task; tasks resume after reconnecting
        ui.setConnection(AppShell.SAVED, display(c));
        ui.deviceState.setText(byHost ? S.ENDED_BY_HOST_DETAIL : S.DISCONNECTED_DETAIL);
        status(byHost ? S.ENDED_BY_HOST_NOTICE : S.DISCONNECTED_NOTICE);
    }
    // ---------- presence: one idle control connection that makes “已连接” truthful ----------
    final AtomicLong presenceSeq = new AtomicLong();
    volatile java.net.Socket presenceSocket;
    void closePresence() {
        java.net.Socket s = presenceSocket;
        presenceSocket = null;
        if (s != null) try { s.close(); } catch (Exception ignored) {}
    }
    void startPresence(Remote.Credentials c) {
        long seq = presenceSeq.incrementAndGet();
        closePresence();
        Thread t = new Thread(() -> presenceLoop(c, seq), "lantern-presence");
        t.setDaemon(true);
        t.start();
    }
    void presenceLoop(Remote.Credentials c, long seq) {
        int failures = 0;
        while (presenceSeq.get() == seq && credentials == c && !isDestroyed()) {
            try (Remote r = new Remote(c, 0, 0, s -> presenceSocket = s)) {
                String caps = Remote.str(Remote.reader(r.call(30, new byte[0])));
                if (caps.contains("session=end-v1")) Remote.sessionEndPin = c.pin;
                // Wi-Fi to mobile data and back: the winning entry tells which path we are on now.
                ConnectionEndpoint used = r.selectedEndpoint, before = via;
                Computers.lastEntry(this, c.pin, used);
                boolean moved = used != null && (before == null || !before.host.equals(used.host) || before.port != used.port);
                via = used;
                if (caps.contains(";endpoints=v1"))
                    try { // keeps a new IPv6 prefix for the next time this phone is away from home
                        Remote.Credentials fresh = c.mergeEndpoints(r.endpoints());
                        if (!fresh.saveEndpointPreferences().equals(c.saveEndpointPreferences())) saveEndpoints(fresh);
                    } catch (Exception ignored) {}
                if (failures > 0 || moved) {
                    boolean announce = moved && before != null && used.publicPath();
                    runOnUiThread(() -> { if (presenceSeq.get() == seq && credentials == c) { ui.setConnection(AppShell.CONNECTED, where(c)); if (announce) status(S.PATH_PUBLIC_NOTICE); } });
                }
                failures = 0;
                while (presenceSeq.get() == seq && credentials == c) {
                    Thread.sleep(10_000);
                    if (presenceSeq.get() != seq || credentials != c) break;
                    long nonce = System.nanoTime();
                    if (Remote.reader(r.call(50, Remote.body(o -> o.writeLong(nonce)))).readLong() != nonce)
                        throw new java.io.IOException("网络探测响应错误");
                }
            } catch (Exception e) {
                if (presenceSeq.get() != seq || credentials != c) return;
                if (Remote.endedByHost(e)) { runOnUiThread(() -> { if (credentials == c) disconnect(true); }); return; }
                failures++;
                if (failures == 2) // one quick retry is silent; after that show we are reconnecting
                    runOnUiThread(() -> { if (presenceSeq.get() == seq && credentials == c) ui.setConnection(AppShell.CONNECTING, display(c)); });
                try { Thread.sleep(RetryPolicy.delayMillis(failures)); } catch (InterruptedException stop) { return; }
            }
        }
    }
    void parentFolder(){String current=path.getText().toString();int slash=current.lastIndexOf('/');ui.files.navigate(slash<0?"":current.substring(0,slash));}
    boolean notificationsAllowed(){return Build.VERSION.SDK_INT<33||checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)==android.content.pm.PackageManager.PERMISSION_GRANTED;}
    void enableTransferNotifications(){if(!notificationsAllowed())requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS},201);else status("传输通知已允许");}
    @Override public void onRequestPermissionsResult(int request,String[] permissions,int[] results){super.onRequestPermissionsResult(request,permissions,results);if(request==REQUEST_CAMERA){if(results.length>0&&results[0]==android.content.pm.PackageManager.PERMISSION_GRANTED)startActivityForResult(new Intent(this,ScanActivity.class),REQUEST_SCAN);else Sheet.info(this,S.SCAN_PAIR,S.CAMERA_DENIED);return;}if(request==201){boolean granted=results.length>0&&results[0]==android.content.pm.PackageManager.PERMISSION_GRANTED;ui.device.notificationsAllowed(granted);status(granted?"传输通知已开启":"未允许通知；可在传输页查看并暂停后台任务");}}
    @Override protected void onNewIntent(Intent intent){super.onNewIntent(intent);setIntent(intent);if(intent.getBooleanExtra("open_transfers",false)){desktopLayout.setFullscreen(false);ui.select(AppShell.TRANSFERS);}handleViewIntent(intent);}
    void networkDiagnostics(){
        Remote.Credentials c=credentials;if(c==null){status("请先连接电脑，再测量实际网络");return;}
        status(S.DIAG_RUNNING);cancelled.set(false);
        job(()->{
            NetworkProbe.Result measured=NetworkProbe.measure(c,cancelled);
            List<NetworkDialer.Probe> entries=probeEntries(c);
            runOnUiThread(()->{
                android.net.ConnectivityManager manager=getSystemService(android.net.ConnectivityManager.class);android.net.NetworkCapabilities link=manager.getNetworkCapabilities(manager.getActiveNetwork());
                String type=link==null?"无默认网络":link.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)?"Wi-Fi":link.hasTransport(android.net.NetworkCapabilities.TRANSPORT_ETHERNET)?"以太网":link.hasTransport(android.net.NetworkCapabilities.TRANSPORT_CELLULAR)?"移动网络":"其他网络";
                if(isDestroyed())return;
                Sheet sheet=new Sheet(this,S.DIAG_TITLE,String.format(Locale.ROOT,S.DIAG_SUBTITLE,type,where(c)));
                LinearLayout grid=Ui.column(this);grid.setPadding(Theme.dp(16),Theme.dp(4),Theme.dp(16),Theme.dp(4));
                diagRow(grid,S.DIAG_HANDSHAKE,String.format(Locale.ROOT,"%.1f ms",measured.handshakeMillis));
                diagRow(grid,S.DIAG_RTT_MEDIAN,String.format(Locale.ROOT,"%.1f ms",measured.medianMillis));
                diagRow(grid,S.DIAG_RTT_RANGE,String.format(Locale.ROOT,"%.1f – %.1f ms",measured.minMillis,measured.maxMillis));
                TransferPacer pacer=TransferPacer.SHARED;double down=pacer.down.rate(),up=pacer.up.rate();
                diagRow(grid,S.DIAG_SPEED,down+up<1024?S.DIAG_IDLE:String.format(Locale.ROOT,S.QUEUE_SPEED,Ui.rate(down),Ui.rate(up)));
                if(pacer.limited())diagRow(grid,S.LIMIT_TITLE,Ui.limit(pacer.userLimit));
                sheet.view(grid).view(Sheet.paragraph(this,S.DIAG_NOTE));
                if(entries.size()>1){
                    LinearLayout list=Ui.column(this);list.setPadding(Theme.dp(16),Theme.dp(4),Theme.dp(16),Theme.dp(4));
                    list.addView(Ui.medium(this,S.DIAG_ENTRIES,15,Theme.INK),Ui.lp(-1,-2));
                    for(NetworkDialer.Probe p:entries)
                        if(p.failure!=NetworkDialer.Failure.CANCELLED)
                            diagRow(list,S.PATH_LABELS[Math.max(0,Math.min(5,p.endpoint.kind))]+"  "+hostText(p.endpoint),probeText(p));
                    sheet.view(list).view(Sheet.paragraph(this,S.DIAG_ENTRIES_NOTE));
                }
                sheet.buttons(null,S.DONE,Ui.SECONDARY,null).show();
            });
        });
    }
    void nextFiles(){offset+=256;browser.refresh();}
    void uploadFiles(){if(credentials==null){status("请先连接电脑");ui.select(AppShell.DEVICE);return;}startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_ALLOW_MULTIPLE,true),101);}
    void uploadDirectory(){if(credentials==null){status("请先连接电脑");ui.select(AppShell.DEVICE);return;}startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),102);}
    void toggleDesktop(){ui.select(AppShell.DESKTOP);if(!subscribed.get()&&credentials==null){status(S.NEED_DEVICE);return;}subscribed.set(!subscribed.get());updateDesktopPresentation();status(subscribed.get()?"桌面已订阅；点击全屏放大":"桌面采集与传输已停止");}
    void toggleDisplay(){display.set(!display.get());updateDesktopPresentation();status(display.get()?"显示画面":"客户端画面已隐藏，采集继续");}
    void renderTransfers(){if(ui==null||transfers==null)return;int active=ui.transfers.render(transfers);ui.transferBadge(active);}
    void showTransfer(String id){for(TransfersPage.TaskItem t:ui.transfers.all)if(t.id.equals(id)){ui.transfers.showTask(t);return;}}
    void updateDesktopPresentation() {
        TransferPacer.SHARED.realtime=subscribed.get()||audioSubscribed;
        if (desktopLayout == null)
            return;
        if (!subscribed.get() || !display.get()) {
            screen.cancelDrag(); if (keyboard != null) keyboard.close();
        }
        desktopLayout.setVisible(subscribed.get() && display.get());
        refreshDesktopUi();
        if (video != null) {
            video.render.set(display.get());
            if (!subscribed.get() || !hardwareDesktop)
                video.stop();
        }
        if (avcView != null)
            avcView.setVisibility(hardwareDesktop && subscribed.get() ? View.VISIBLE : View.GONE);
    }
    void selectMonitor(int monitor){if(monitor<0||monitor>=32)return;screen.cancelDrag();controlRequested=false;control.set(false);inputEpoch.incrementAndGet();closeInput();controlLabels();selectedMonitor=monitor;monitorGeneration.incrementAndGet();videoProfile=qualityProfile();liveQuality=null;constrainedSince=0;video.stop();Remote old=desktop;desktop=null;if(old!=null)try{old.abort();}catch(IOException ignored){}screen.zoom=1;screen.panX=screen.panY=0;screen.imageWidth=screen.imageHeight=0;screen.setImageDrawable(null);screen.viewport.valid=false;screen.updateTransform();updateDesktopPresentation();status("已选择显示器 "+(monitor+1)+"；控制已关闭，请确认画面后重新开启");}
    void showDesktopTools(){
        new Sheet(this,S.TOOLS_TITLE,S.TOOLS_SUBTITLE)
            .item(Glyph.CLIPBOARD,S.CLIP_PUSH,S.CLIP_PUSH_DETAIL,()->desktopTools.push(credentials))
            .item(Glyph.PASTE,S.CLIP_PULL,S.CLIP_PULL_DETAIL,()->desktopTools.pull(credentials))
            .item(Glyph.MONITORS,S.TOOL_MONITOR,String.format(Locale.ROOT,S.MONITOR_N,selectedMonitor+1),()->desktopTools.queryMonitors(credentials))
            .item(Glyph.COMMAND,S.TOOL_SHORTCUTS,S.SHORTCUTS_DETAIL,this::showShortcuts)
            .item(Glyph.CLOCK,S.TOOL_TIMING,timeline.synchronize?S.TIMING_SYNC:S.TIMING_FAST,this::showTiming)
            .unsupported(Glyph.SHIELD,S.PRIVACY_TITLE,S.PRIVACY_SHORT)
            .show();
    }
    void showClipboard(){
        new Sheet(this,S.TOOL_CLIPBOARD,S.CLIP_SUBTITLE)
            .item(Glyph.CLIPBOARD,S.CLIP_PUSH,S.CLIP_PUSH_DETAIL,()->desktopTools.push(credentials))
            .item(Glyph.PASTE,S.CLIP_PULL,S.CLIP_PULL_DETAIL,()->desktopTools.pull(credentials))
            .show();
    }
    void showShortcuts(){
        String[] labels={S.KEY_COPY,S.KEY_PASTE,S.KEY_ALL,S.KEY_SWITCH,S.KEY_DESKTOP,S.KEY_CLOSE};
        String[] combos={"Ctrl + C","Ctrl + V","Ctrl + A","Alt + Tab","Win + D","Alt + F4"};
        int[] modifiers={1,1,1,2,8,2},keys={67,86,65,9,68,115};
        Sheet sheet=new Sheet(this,S.SHORTCUTS_TITLE,S.SHORTCUTS_SUBTITLE);
        for(int i=0;i<labels.length;i++){final int n=i;sheet.item(-1,labels[i],combos[i],()->desktopTools.shortcut(credentials,modifiers[n],keys[n]));}
        sheet.show();
    }
    void showTiming(){
        new Sheet(this,S.TOOL_TIMING,S.TIMING_SUBTITLE)
            .option(S.TIMING_FAST,S.TIMING_FAST_DETAIL,!timeline.synchronize,()->setTiming(false))
            .option(S.TIMING_SYNC,S.TIMING_SYNC_DETAIL,timeline.synchronize,()->setTiming(true))
            .show();
    }
    void setTiming(boolean synchronize){
        if(timeline.synchronize==synchronize)return;
        timeline.synchronize=synchronize;video.stop();
        status(timeline.synchronize?"声音同步：按音频设备时钟等待画面，最多350ms；控制操作建议选择响应优先":"响应优先：画面不等待音频，可降低控制延迟；声音仍独立播放");
        refreshDesktopUi();
    }
    void showMonitors(List<DesktopTools.Monitor> values){
        Sheet sheet=new Sheet(this,S.MONITOR_TITLE,S.MONITOR_SUBTITLE);
        for(DesktopTools.Monitor m:values){final int index=m.index;sheet.option(String.format(Locale.ROOT,S.MONITOR_N,m.index+1)+"  "+m.name,m.width+" × "+m.height,m.index==selectedMonitor,()->selectMonitor(index));}
        sheet.show();
    }
    void showLicenses(){
        String[] files;try{files=getAssets().list("licenses");}catch(IOException e){files=null;}
        if(files==null||files.length==0){status(S.LICENSES_MISSING);return;}
        Sheet sheet=new Sheet(this,S.LICENSES,S.LICENSES_SUBTITLE);
        for(String name:files)sheet.item(Glyph.LICENSE,name.replace(".txt",""),null,()->showLicense(name));
        sheet.show();
    }
    void showLicense(String name){
        job(()->{
            List<String> lines=new ArrayList<>();
            try(BufferedReader reader=new BufferedReader(new InputStreamReader(getAssets().open("licenses/"+name),java.nio.charset.StandardCharsets.UTF_8))){String line;while((line=reader.readLine())!=null)lines.add(line);}
            runOnUiThread(()->{
                if(isDestroyed())return;
                Sheet sheet=new Sheet(this,name,null);
                ListView text=new ListView(this);text.setDivider(null);text.setSelector(new android.graphics.drawable.ColorDrawable(0));
                text.setAdapter(new ArrayAdapter<String>(this,0,lines){@Override public View getView(int i,View old,ViewGroup parent){TextView t=old instanceof TextView?(TextView)old:Ui.text(MainActivity.this,"",12,Theme.MUTED);t.setTypeface(Theme.MONO);t.setPadding(Theme.dp(16),0,Theme.dp(16),0);t.setText(getItem(i));return t;}});
                sheet.body.addView(text,Ui.lp(-1,(int)(getResources().getDisplayMetrics().heightPixels*0.6f)));
                sheet.buttons(null,S.DONE,Ui.SECONDARY,null).show();
            });
        });
    }
    void diagRow(LinearLayout parent,String label,String value){
        LinearLayout r=Ui.row(this);r.setPadding(0,Theme.dp(8),0,Theme.dp(8));
        r.addView(Ui.text(this,label,15,Theme.MUTED),Ui.weight(1));
        r.addView(Ui.numeric(Ui.medium(this,value,15,Theme.INK)));
        parent.addView(r,Ui.lp(-1,-2));
    }
    void refreshDesktopUi(){
        if(ui==null)return;
        int controlState=control.get()?2:controlRequested?1:0;
        ui.desktop.update(subscribed.get(),display.get(),controlRequested,control.get(),audioState,profileLabel(),selectedMonitor,timeline.synchronize);
        if(desktopLayout!=null){desktopLayout.setControl(controlState);desktopLayout.setAudio(audioState);}
        linkChanged();
        preferRefresh(hardwareDesktop && subscribed.get() && display.get() && desktopLayout != null && desktopLayout.visible ? qualityFps : 0);
    }
    /**
     * Asks for a display mode while the desktop picture is up: twice the stream rate where the panel
     * has it (60 fps on 120 Hz absorbs network jitter), else its fastest mode; the system default
     * otherwise. This sets the panel mode only: ColorOS's adaptive refresh still presents an app that
     * has not been touched for a few seconds at 60 Hz (measured on the compositor), whatever it asks.
     */
    int preferredMode = -1;
    void preferRefresh(int fps) {
        int want = 0;
        try {
            if (fps > 0) {
                Display d = getWindowManager().getDefaultDisplay();
                Display.Mode current = d.getMode(), best = null, fastest = null;
                for (Display.Mode m : d.getSupportedModes()) {
                    if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
                    if (fastest == null || m.getRefreshRate() > fastest.getRefreshRate()) fastest = m;
                    if (m.getRefreshRate() >= Math.min(fps * 2, 120) - 1 && (best == null || m.getRefreshRate() < best.getRefreshRate())) best = m;
                }
                Display.Mode chosen = best != null ? best : fastest;
                if (chosen != null) want = chosen.getModeId();
            }
        } catch (RuntimeException unavailable) { want = 0; }
        if (want == preferredMode) return;
        preferredMode = want;
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.preferredDisplayModeId = want;
        getWindow().setAttributes(lp);
    }
    // ---- Picture quality: resolution (4K / 2K / 1080p / 720p, "自动" last) × frame rate (120 / 60 / 30).
    // Choices above the computer screen or the phone decoder are allowed; the host sends at the real
    // limit and the option says which limit applies. Changes apply online on v3+ hosts.
    static final int[] Q_WIDTHS = {3840, 2560, 1920, 1280}, Q_FPS = {120, 60, 30};
    volatile int qualityWidth = 1920, qualityFps = 60; volatile boolean qualityAuto;
    List<DesktopSession.Display> hostDisplays; Remote.Credentials hostDisplaysFor; boolean hostDisplaysLoading;
    static volatile List<DecoderCapabilities.Limit> phoneDecoders;
    DesktopSession.Status liveQuality; long constrainedSince;
    static String tierName(int width) {
        return width >= 3840 ? "4K" : width >= 2560 ? "2K" : width >= 1920 ? "1080p" : width >= 1280 ? "720p" : width + "p";
    }
    static String screenName(int width, int height) {
        for (int w : Q_WIDTHS) if (width == w && height == w * 9 / 16) return tierName(w);
        return width + "×" + height;
    }
    /** Bitrate only matters to hosts older than desktop v3; newer hosts choose their own budget. */
    static int legacyBitrate(int width, int fps) {
        int base = width >= 3840 ? 30_000_000 : width >= 2560 ? 20_000_000 : width >= 1920 ? 12_000_000 : 8_000_000;
        return fps <= 30 ? base * 2 / 3 : base;
    }
    AvcDesktop.Profile qualityProfile() {
        int width = qualityAuto ? 3840 : qualityWidth;
        return new AvcDesktop.Profile(width, qualityFps, legacyBitrate(width, qualityFps), selectedMonitor, qualityAuto);
    }
    void loadQualityLimits(Remote.Credentials c) {
        if (c == null || hostDisplaysFor == c || hostDisplaysLoading) return;
        hostDisplaysLoading = true;
        try { work.execute(() -> {
            List<DesktopSession.Display> displays = null;
            try { displays = DesktopSession.limits(c); } catch (Exception older) { /* pre-v3 host: no limits */ }
            if (phoneDecoders == null) try { phoneDecoders = DecoderCapabilities.query(); } catch (RuntimeException ignored) {}
            List<DesktopSession.Display> found = displays;
            runOnUiThread(() -> { hostDisplaysLoading = false; if (credentials == c) { hostDisplays = found; hostDisplaysFor = c; } });
        }); } catch (RejectedExecutionException busy) { hostDisplaysLoading = false; }
    }
    DesktopSession.Display hostDisplay() {
        List<DesktopSession.Display> list = hostDisplaysFor == credentials ? hostDisplays : null;
        if (list == null) return null;
        for (DesktopSession.Display d : list) if (d.index == selectedMonitor) return d;
        return list.isEmpty() ? null : list.get(0);
    }
    /** Largest standard width the phone decodes at this frame rate (0 = unknown). */
    static int phoneMaxWidth(int fps, boolean hevc) {
        List<DecoderCapabilities.Limit> limits = phoneDecoders;
        if (limits == null) return 0;
        int best = 0;
        for (DecoderCapabilities.Limit l : limits)
            if (l.fps >= fps && (l.mime.equals("video/avc") || hevc)) best = Math.max(best, l.width);
        return best;
    }
    static int phoneMaxFps(int width, boolean hevc) {
        List<DecoderCapabilities.Limit> limits = phoneDecoders;
        if (limits == null) return 0;
        int best = 0;
        for (DecoderCapabilities.Limit l : limits)
            if (l.width >= width && (l.mime.equals("video/avc") || hevc)) best = Math.max(best, l.fps);
        return best;
    }
    String resolutionDetail(int width) {
        DesktopSession.Display host = hostDisplay();
        boolean hevc = host == null || host.hevc;
        List<String> limits = new ArrayList<>();
        if (host != null && host.width < width) limits.add(String.format(S.Q_HOST_MAX, screenName(host.width, host.height)));
        int phone = phoneMaxWidth(Math.min(qualityFps, 60), hevc);
        if (phone > 0 && phone < width) limits.add(String.format(S.Q_PHONE_MAX, tierName(phone)));
        String size = width + "×" + (width * 9 / 16);
        return limits.isEmpty() ? size : size + "，" + String.join("，", limits) + S.Q_SENT_AT_LIMIT;
    }
    String fpsDetail(int fps) {
        DesktopSession.Display host = hostDisplay();
        boolean hevc = host == null || host.hevc;
        List<String> limits = new ArrayList<>();
        if (host != null && host.refresh > 0 && host.refresh < fps) limits.add(String.format(S.Q_HOST_FPS_MAX, host.refresh));
        int width = qualityAuto ? 1280 : qualityWidth;
        int phone = phoneMaxFps(width, hevc);
        if (phone > 0 && phone < fps) limits.add(String.format(S.Q_PHONE_FPS_MAX, tierName(width), phone));
        String base = fps == 120 ? S.Q_FPS_120 : fps == 60 ? S.Q_FPS_60 : S.Q_FPS_30;
        return limits.isEmpty() ? base : String.join("，", limits) + S.Q_SENT_AT_LIMIT;
    }
    void applyQuality(String message) {
        hardwareDesktop = true;
        videoProfile = qualityProfile(); liveQuality = null; constrainedSince = 0;
        getSharedPreferences("device", 0).edit().putBoolean("desktop_h264", true)
            .putInt("video_width", qualityWidth).putInt("video_fps", qualityFps).putBoolean("video_auto", qualityAuto).apply();
        updateDesktopPresentation();
        refreshDesktopUi();
        status(message);
    }
    void showQuality() {
        loadQualityLimits(credentials);
        Sheet sheet = new Sheet(this, S.QUALITY_TITLE, S.QUALITY_SUBTITLE);
        sheet.section(S.Q_SECTION_RESOLUTION);
        for (int width : Q_WIDTHS)
            sheet.option(tierName(width), resolutionDetail(width), hardwareDesktop && !qualityAuto && qualityWidth == width, () -> {
                qualityAuto = false; qualityWidth = width; applyQuality(String.format(S.Q_CHANGED, tierName(width) + qualityFps));
            });
        sheet.option(S.Q_AUTO, String.format(S.Q_AUTO_DETAIL, qualityFps), hardwareDesktop && qualityAuto, () -> {
            qualityAuto = true; applyQuality(S.Q_AUTO_CHANGED);
        });
        sheet.section(S.Q_SECTION_FPS);
        for (int fps : Q_FPS)
            sheet.option(fps + " " + S.Q_FPS_UNIT, fpsDetail(fps), hardwareDesktop && qualityFps == fps, () -> {
                qualityFps = fps; applyQuality(String.format(S.Q_CHANGED, (qualityAuto ? S.Q_AUTO : tierName(qualityWidth)) + fps));
            });
        String[] titles = {S.Q_FAST, S.Q_BALANCED, S.Q_SHARP};
        String[] details = {S.Q_FAST_DETAIL, S.Q_BALANCED_DETAIL, S.Q_SHARP_DETAIL};
        sheet.section(S.Q_SECTION_JPEG);
        for (int i = 0; i < titles.length; i++) {
            final int index = i;
            sheet.option(titles[i], details[i], !hardwareDesktop && desktopProfile == PROFILES[i], () -> {
                hardwareDesktop = false; desktopProfile = PROFILES[index];
                getSharedPreferences("device", 0).edit().putBoolean("desktop_h264", false).putInt("desktop_profile", index).apply();
                updateDesktopPresentation(); refreshDesktopUi();
                status("画质已调整；实际更新率显示在画面左上角");
            });
        }
        sheet.show();
    }
    /** One step down from a fixed choice that the network cannot carry (banner action). */
    void lowerQuality() {
        for (int i = 0; i < Q_WIDTHS.length - 1; i++)
            if (qualityWidth >= Q_WIDTHS[i]) { qualityWidth = Q_WIDTHS[i + 1]; applyQuality(String.format(S.Q_CHANGED, tierName(qualityWidth) + qualityFps)); return; }
        if (qualityFps > 30) { qualityFps = qualityFps == 120 ? 60 : 30; applyQuality(String.format(S.Q_CHANGED, tierName(qualityWidth) + qualityFps)); }
    }
    String lowerQualityLabel() {
        if (qualityAuto) return null;
        for (int i = 0; i < Q_WIDTHS.length - 1; i++) if (qualityWidth >= Q_WIDTHS[i]) return String.format(S.Q_LOWER, tierName(Q_WIDTHS[i + 1]) + qualityFps);
        return qualityFps > 30 ? String.format(S.Q_LOWER, tierName(qualityWidth) + (qualityFps == 120 ? 60 : 30)) : null;
    }
    void quality(DesktopSession.Status s) {
        liveQuality = s;
        boolean short_ = !qualityAuto && (s.constrained || s.dropping);
        if (!short_) constrainedSince = 0; else if (constrainedSince == 0) constrainedSince = SystemClock.uptimeMillis();
        refreshDesktopUi();
    }
    String profileLabel(){
        if (!hardwareDesktop) return desktopProfile.width + "px";
        DesktopSession.Status s = liveQuality;
        String chosen = qualityAuto ? S.Q_AUTO : tierName(qualityWidth) + qualityFps;
        if (s == null || s.width <= 0) return chosen;
        String actual = tierName(s.width) + s.fps;
        String path = "tcp".equalsIgnoreCase(s.path) ? " · TCP" : ""; // marks a UDP fallback
        return (qualityAuto ? S.Q_AUTO + " " + actual : actual.equals(chosen) ? chosen : chosen + "→" + actual) + path;
    }
    void controlLabels() {
        if (screen != null) screen.controlEnabled = control.get();
        if (!control.get()) {
            if (screen != null) screen.cancelDrag();
            draggingRemote = false; inputUi.removeCallbacks(dragHeartbeat);
            if (keyboard != null) keyboard.close();
            disarmRightClick();
        }
        refreshDesktopUi();
    }
    /** Toolbar “右键”: arms the next tap as a right click. Needs control, like every other input. */
    void toggleRightClick() {
        if (!control.get()) { status(S.RIGHT_NEED_CONTROL); return; }
        rightArmed = !rightArmed;
        if (desktopLayout != null) desktopLayout.setRightArmed(rightArmed);
        if (rightArmed) status(S.RIGHT_ARMED);
    }
    void disarmRightClick() {
        if (!rightArmed) return;
        rightArmed = false;
        if (desktopLayout != null) desktopLayout.setRightArmed(false);
    }
    void audioState(int value) { audioState = value; refreshDesktopUi(); }
    void toggleAudio() {
        TransferPacer.SHARED.realtime=true;
        if(audioSubscribed) { audioSubscribed=false; systemAudio.stop(); audioState(AUDIO_OFF); status("系统音频已停止"); return; }
        if(credentials==null) { status(S.NEED_DEVICE); return; }
        audioSubscribed=true; audioState(AUDIO_CONNECTING); systemAudio.start(credentials);
    }
    // ---- Control: one reliable InputChannel per control session. The channel reconnects, replays
    // unacknowledged events and coalesces moves; nothing here turns control off because of the network.
    InputChannel input;                                        // UI thread
    volatile String lastInputResult = "";                      // diagnostics: seq:code:reason
    final ArrayDeque<Object[]> inputBacklog = new ArrayDeque<>(); // {op, body, tag, move} waiting for capacity
    static final Object TAG_KEY = new Object(), TAG_TEXT = new Object(), TAG_DRAG_END = new Object();
    /** Left clicks probe the keyboard afterwards; right clicks only report a refusal. */
    static final class Click { final long request; final boolean left; Click(long r, boolean l) { request = r; left = l; } }
    static final class Probe { final long request; final boolean retried; Probe(long r, boolean t) { request = r; retried = t; } }
    void closeInput() {
        InputChannel old = input; input = null;
        inputBacklog.clear(); inputUi.removeCallbacks(drainInput);
        controlLink = null;
        if (old != null) old.close();
        linkChanged();
    }
    void openInput(Remote.Credentials c) {
        closeInput();
        long epoch = inputEpoch.get();
        input = new InputChannel(c, new InputChannel.Events() {
            public void state(InputChannel.State s) {
                runOnUiThread(() -> {
                    if (inputEpoch.get() != epoch || input == null) return;
                    if (s.terminal) {
                        // Revoked trust, an ended session or an uncertain replay: never retried silently.
                        controlRequested = false; control.set(false); inputEpoch.incrementAndGet(); closeInput(); controlLabels();
                        status(S.CONTROL_STOPPED);
                        return;
                    }
                    controlLink = s.phase == InputChannel.Phase.NORMAL ? null : s; controlLinkAt = SystemClock.uptimeMillis();
                    linkChanged();
                });
            }
            public void result(long seq, int code, String reason, byte[] payload, Object tag) {
                lastInputResult = seq + ":" + code + ":" + reason;
                if (tag == null) return; // pointer moves
                if (code != 0) { if (inputEpoch.get() == epoch) status(S.CONTROL_REFUSED + reason); return; }
                if (tag instanceof Click) {
                    Click click = (Click) tag;
                    if (!click.left) return;
                    controlClicksCompleted.incrementAndGet();
                    inputUi.postDelayed(() -> probeKeyboard(epoch, c, click.request, false), 160);
                } else if (tag instanceof Probe) {
                    Probe probe = (Probe) tag;
                    int flags;
                    try { flags = Remote.reader(payload).readInt(); } catch (IOException bad) { return; }
                    lastFocusFlags = flags;
                    runOnUiThread(() -> {
                        if (inputEpoch.get() != epoch || !control.get() || !subscribed.get() || !display.get() || keyboardRequestId.get() != probe.request) return;
                        if ((flags & 1) != 0) keyboard.open((flags & 2) != 0);
                        else {
                            keyboard.close();
                            if (!probe.retried) inputUi.postDelayed(() -> probeKeyboard(epoch, c, probe.request, true), 350);
                        }
                    });
                } else if (tag == TAG_DRAG_END) dragEndsCompleted.incrementAndGet();
                else if (tag == TAG_TEXT) textCommitsCompleted.incrementAndGet();
            }
        });
    }
    static boolean pointerMove(int op, byte[] body) {
        return body.length >= 4 && ((op == 36 && java.nio.ByteBuffer.wrap(body).getInt(0) == 2) || (op == 22 && java.nio.ByteBuffer.wrap(body).getInt(0) == 0));
    }
    /** UI thread. When the channel is full the event waits here, in order; only the newest pointer move is kept. */
    void sendInput(int op, byte[] body, Object tag) {
        InputChannel channel = input;
        if (channel == null) return;
        boolean move = pointerMove(op, body);
        if (inputBacklog.isEmpty()) {
            try { if (channel.submit(op, body, tag)) return; }
            catch (IOException invalid) { status(invalid.getMessage()); return; }
            inputUi.postDelayed(drainInput, 30);
        } else {
            Object[] tail = inputBacklog.peekLast();
            if (move && (Boolean) tail[3]) inputBacklog.removeLast();
        }
        inputBacklog.addLast(new Object[]{op, body, tag, move});
    }
    final Runnable drainInput = new Runnable() {
        public void run() {
            InputChannel channel = input;
            if (channel == null) { inputBacklog.clear(); return; }
            while (!inputBacklog.isEmpty()) {
                Object[] next = inputBacklog.peekFirst();
                try { if (!channel.submit((Integer) next[0], (byte[]) next[1], next[2])) { inputUi.postDelayed(this, 30); return; } }
                catch (IOException invalid) { /* bounded payloads; cannot happen for queued events */ }
                inputBacklog.pollFirst();
            }
        }
    };
    /** Test barrier: returns once every submitted control event has been acknowledged by the computer. */
    void awaitInput(long millis) throws Exception {
        long end = SystemClock.uptimeMillis() + millis;
        while (true) {
            CountDownLatch read = new CountDownLatch(1); boolean[] idle = {false};
            runOnUiThread(() -> { InputChannel ch = input; idle[0] = ch == null || (inputBacklog.isEmpty() && ch.pending() == 0); read.countDown(); });
            read.await();
            if (idle[0]) { Thread.sleep(30); return; }
            if (SystemClock.uptimeMillis() > end) throw new TimeoutException("control input not acknowledged");
            Thread.sleep(20);
        }
    }
    static byte[] pointerBody(int a, int b, int c) {
        return java.nio.ByteBuffer.allocate(12).putInt(a).putInt(b).putInt(c).array();
    }

    // ---- Link banner: silent under a second, "recovering" while any live channel recovers,
    // "disconnected" plus a reconnect button after 30 seconds. Never a dialog.
    ConnectionStatus.Snapshot videoLink, audioLink; InputChannel.State controlLink;
    long videoLinkAt, audioLinkAt, controlLinkAt;
    boolean videoSeen, audioSeen; // a channel only "recovers" after it has worked once
    final Runnable linkTick = this::linkChanged;
    static long elapsed(long recovering, long at, long now) { return recovering + Math.max(0, now - at); }
    void linkChanged() {
        inputUi.removeCallbacks(linkTick);
        if (desktopLayout == null) return;
        if (!subscribed.get() || !hardwareDesktop) { videoSeen = false; videoLink = null; }
        if (!audioSubscribed) { audioSeen = false; audioLink = null; }
        long now = SystemClock.uptimeMillis(), worst = 0; boolean lost = false;
        if (videoSeen && videoLink != null && videoLink.phase != ConnectionStatus.Phase.NORMAL) {
            long e = elapsed(videoLink.recoveringMillis, videoLinkAt, now); worst = Math.max(worst, e);
            lost |= videoLink.phase == ConnectionStatus.Phase.DISCONNECTED || e >= 30_000;
        }
        if (audioSeen && audioLink != null && audioLink.phase != ConnectionStatus.Phase.NORMAL) {
            long e = elapsed(audioLink.recoveringMillis, audioLinkAt, now); worst = Math.max(worst, e);
            lost |= audioLink.phase == ConnectionStatus.Phase.DISCONNECTED || e >= 30_000;
        }
        if (controlRequested && input != null && controlLink != null && controlLink.phase != InputChannel.Phase.NORMAL) {
            long e = elapsed(controlLink.recoveringMillis, controlLinkAt, now); worst = Math.max(worst, e);
            lost |= controlLink.phase == InputChannel.Phase.DISCONNECTED || e >= 30_000;
        }
        // A fixed choice the network cannot carry: suggest one step down, never switch by itself.
        String lower = lowerQualityLabel();
        boolean short_ = hardwareDesktop && videoSeen && constrainedSince != 0 && now - constrainedSince >= 3000;
        if (lost) desktopLayout.link(S.LINK_LOST, S.LINK_RECONNECT, this::reconnectLinks);
        else if (worst >= 3000) desktopLayout.link(String.format(Locale.ROOT, S.LINK_RECOVERING_FOR, worst / 1000), null, null);
        else if (worst >= 1000) desktopLayout.link(S.LINK_RECOVERING, null, null);
        else if (short_) desktopLayout.link(String.format(S.Q_SHORT, tierName(qualityWidth) + qualityFps), lower, lower == null ? null : this::lowerQuality);
        else desktopLayout.link(null, null, null);
        if (worst > 0 || (constrainedSince != 0 && !short_)) inputUi.postDelayed(linkTick, 500);
    }
    /** “重新连接”: restarts every channel the user has on, with the same authorization. */
    void reconnectLinks() {
        Remote.Credentials c = credentials;
        if (c == null) { status(S.NEED_DEVICE); return; }
        videoLink = audioLink = null; controlLink = null;
        if (subscribed.get() && hardwareDesktop) { video.stop(); videoSeen = false; } // the frame loop starts it again
        if (audioSubscribed) { systemAudio.stop(); audioSeen = false; audioState(AUDIO_CONNECTING); systemAudio.start(c); }
        if (controlRequested && control.get()) openInput(c);
        linkChanged();
    }
    void toggleControl() {
        long epoch = inputEpoch.incrementAndGet();
        if (controlRequested) {
            controlRequested = false; control.set(false); closeInput(); controlLabels(); status("控制已关闭"); return;
        }
        Remote.Credentials c = credentials;
        if (c == null) { status(S.NEED_DEVICE); return; }
        controlRequested = true; controlLabels();
        try { work.execute(() -> {
            try (Remote probe = new Remote(c, 8, 0)) {
                String capabilities = Remote.str(Remote.reader(probe.call(30, new byte[0])));
                if (!capabilities.contains("gesture-v1") || !capabilities.contains("unicode-ime-v1")) throw new IOException("主机需要更新才能使用拖拽与输入法");
                runOnUiThread(() -> {
                    if (inputEpoch.get() != epoch || credentials != c || !controlRequested) return;
                    openInput(c); control.set(true); controlLabels();
                    status("控制已开启：单指拖拽、双击打开、双指轻点右键，双指缩放和平移");
                });
            } catch (Exception error) {
                String why = error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage();
                runOnUiThread(() -> {
                    if (inputEpoch.get() != epoch) return;
                    controlRequested = false; control.set(false); controlLabels(); status("无法开启控制：" + why);
                });
                status(why); // lets an ended session reach disconnect()
            }
        }); } catch (RejectedExecutionException busy) { controlRequested = false; controlLabels(); status("操作队列已满"); }
    }
    void desktopTap(float touchX, float touchY) {
        if (rightArmed) { desktopRightClick(touchX, touchY); return; }
        desktopClick(touchX, touchY, false);
    }
    void desktopRightClick(float touchX, float touchY) {
        disarmRightClick();
        if (!control.get() || !subscribed.get() || !display.get()) return;
        int[] point = screen.viewport.pointer(touchX, touchY);
        if (point == null) return; // Letterbox or outside the picture.
        // Op 52 = right_click: x, y, monitor (same body as click).
        sendInput(52, pointerBody(point[0], point[1], selectedMonitor), new Click(keyboardRequestId.incrementAndGet(), false));
    }
    void desktopClick(float touchX, float touchY, boolean twice) {
        if (!control.get() || !subscribed.get() || !display.get()) return;
        int[] point = screen.viewport.pointer(touchX, touchY);
        if (point == null) return; // Ignore letterboxes and coordinates outside the view.
        sendInput(twice ? 37 : 35, pointerBody(point[0], point[1], selectedMonitor), new Click(keyboardRequestId.incrementAndGet(), true));
    }
    void probeKeyboard(long epoch, Remote.Credentials c, long request, boolean retried) {
        if (!control.get() || draggingRemote || credentials != c || inputEpoch.get() != epoch || keyboardRequestId.get() != request) return;
        sendInput(40, new byte[0], new Probe(request, retried));
    }
    void showKeyboard() {
        if (!control.get()) { status("请先开启控制，再点击电脑输入框"); return; }
        keyboardRequestId.incrementAndGet(); keyboard.open((lastFocusFlags & 2)!=0);
    }
    boolean desktopDragBegin(float x, float y) {
        if (!control.get() || !subscribed.get() || !display.get()) return false;
        int[] point = screen.viewport.pointer(x, y); if (point == null) return false;
        keyboardRequestId.incrementAndGet(); keyboard.close(); draggingRemote = true;
        dragX = x; dragY = y;
        sendPointer(1, point, TAG_KEY);
        inputUi.removeCallbacks(dragHeartbeat); inputUi.postDelayed(dragHeartbeat, 250);
        return true;
    }
    void sendPointer(int phase, int[] point, Object tag) {
        sendInput(36, java.nio.ByteBuffer.allocate(16).putInt(phase).putInt(point[0]).putInt(point[1]).putInt(selectedMonitor).array(), tag);
    }
    void desktopDragMove(float x, float y) {
        if (!draggingRemote || !control.get()) return;
        dragX = x; dragY = y;
        int[] point = screen.viewport.dragPointer(x, y); if (point == null) return;
        sendPointer(2, point, null);
    }
    void desktopDragEnd(float x, float y, boolean cancel) {
        if (!draggingRemote) return;
        draggingRemote = false; inputUi.removeCallbacks(dragHeartbeat);
        int[] point = screen.viewport.dragPointer(x, y); if (point == null) point = new int[]{0, 0};
        sendPointer(cancel ? 4 : 3, point, TAG_DRAG_END);
    }
    void desktopText(String value) {
        if (!control.get() || !subscribed.get() || !display.get() || value.isEmpty()) return;
        // Preserve ordering across text and Enter/Tab. Bounded chunks do not split surrogate pairs.
        if (value.length() > 4096) { status("单次输入过长，请分段输入"); return; }
        StringBuilder chunk = new StringBuilder();
        for (int i=0; i<value.length();) {
            int ch=value.codePointAt(i); i+=Character.charCount(ch);
            if (ch=='\r' && i<value.length() && value.charAt(i)=='\n') i++;
            if (ch=='\n' || ch=='\r' || ch=='\t') {
                if (chunk.length()>0) { sendText(chunk.toString()); chunk.setLength(0); }
                desktopKey(ch=='\t' ? 9 : 13, 1);
            } else if (ch>=32 && ch!=127) {
                chunk.appendCodePoint(ch);
                if (chunk.length()>=256) { sendText(chunk.toString()); chunk.setLength(0); }
            }
        }
        if (chunk.length()>0) sendText(chunk.toString());
    }
    void sendText(String value) {
        try { sendInput(38, Remote.body(o -> Remote.str(o, value)), TAG_TEXT); } catch (Exception invalid) { status(invalid.getMessage()); }
    }
    void desktopKey(int vk, int count) {
        if (!control.get() || !subscribed.get() || !display.get()) return;
        sendInput(39, java.nio.ByteBuffer.allocate(8).putInt(vk).putInt(count).array(), TAG_KEY);
    }
    @Override
    public void onBackPressed() {
        if (keyboard != null && keyboard.accepting) { keyboard.close(); return; }
        if (desktopLayout != null && desktopLayout.fullscreen)
            desktopLayout.setFullscreen(false);
        else if(ui.selected==AppShell.FILES&&browser.selecting)ui.files.setSelecting(false);
        else if(ui.selected==AppShell.FILES&&ui.files.searchQuery!=null){ui.files.searchQuery=null;offset=0;browser.refresh();ui.files.renderCrumbs();}
        else if(ui.selected==AppShell.DESKTOP||ui.selected==AppShell.TRANSFERS)ui.select(AppShell.FILES);
        else if(ui.selected==AppShell.FILES&&!path.getText().toString().isEmpty())parentFolder();
        else super.onBackPressed();
    }
    @Override public void onConfigurationChanged(android.content.res.Configuration config){super.onConfigurationChanged(config);if(ui!=null)ui.configurationChanged(config);}
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && desktopLayout != null)
            desktopLayout.systemBars();
    }
    void refresh() throws Exception {
        if(credentials==null)throw new IOException("请先配对");
        runOnUiThread(() -> browser.refresh());
    }
    void open(Uri uri, String name) {
        try {
            Intent intent = new Intent(Intent.ACTION_VIEW)
                                .setDataAndType(uri, FilesProvider.mime(name))
                                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            intent.setClipData(ClipData.newRawUri("Lantern read-only", uri));
            startActivity(Intent.createChooser(intent, S.CHOOSER_TITLE));
        } catch (ActivityNotFoundException e) {
            status("没有兼容的应用，请下载后用其他播放器或阅读器打开");
        }
    }
    // ---- Movies: "原画" (read the original) or "适配本机屏幕" (the computer transcodes to this screen).
    // Over a public path adaptation is the default, on the LAN the original.
    static boolean isVideo(String name) { return FilesProvider.mime(name).startsWith("video/"); }
    boolean onMobileData() {
        try {
            android.net.ConnectivityManager m = getSystemService(android.net.ConnectivityManager.class);
            android.net.NetworkCapabilities n = m.getNetworkCapabilities(m.getActiveNetwork());
            return n != null && n.hasTransport(android.net.NetworkCapabilities.TRANSPORT_CELLULAR) &&
                   !n.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI);
        } catch (RuntimeException unknown) { return false; }
    }
    /** Adds the two playback rows to a file sheet, the default first and marked. */
    void movieOptions(Sheet sheet, Remote.Entry e, String remote) {
        // The path to the computer decides: public (outside, mobile data) adapts, LAN plays the original.
        // The phone's own default network can be cellular while it shares a hotspot with the computer.
        ConnectionEndpoint path = via;
        boolean adaptFirst = path != null ? path.publicPath() : onMobileData();
        Runnable original = () -> openRemote(e, remote, 0), adapted = () -> chooseTracks(e, remote);
        String o = S.MOVIE_ORIGINAL + (adaptFirst ? "" : S.MOVIE_DEFAULT), a = S.MOVIE_ADAPTED + (adaptFirst ? S.MOVIE_DEFAULT : "");
        if (adaptFirst) { sheet.item(Glyph.PHONE, a, S.MOVIE_ADAPTED_DETAIL, adapted); sheet.item(Glyph.PLAY, o, S.MOVIE_ORIGINAL_DETAIL, original); }
        else { sheet.item(Glyph.PLAY, o, S.MOVIE_ORIGINAL_DETAIL, original); sheet.item(Glyph.PHONE, a, S.MOVIE_ADAPTED_DETAIL, adapted); }
    }
    static boolean chinese(MediaPlayback.Track t) {
        String l = t.language.toLowerCase(Locale.ROOT), title = t.title;
        return l.startsWith("chi") || l.startsWith("zho") || l.startsWith("zh") || l.equals("chs") || l.equals("cht") ||
               title.contains("中") || title.contains("简") || title.contains("繁") || title.toLowerCase(Locale.ROOT).contains("chs");
    }
    static String trackName(MediaPlayback.Track t, int n) {
        String name = !t.title.isEmpty() ? t.title : !t.language.isEmpty() ? t.language : (t.kind == 1 ? S.MOVIE_AUDIO : S.MOVIE_SUBTITLE) + " " + n;
        return name + " · " + t.codec.toUpperCase(Locale.ROOT);
    }
    /** Queries the tracks, then lets the user pick audio and an ASS subtitle (burnt in) before playing. */
    void chooseTracks(Remote.Entry e, String remote) {
        Remote.Credentials c = credentials;
        if (c == null) { status(S.NEED_DEVICE); return; }
        status(S.MOVIE_QUERYING);
        job(() -> {
            MediaPlayback.Tracks tracks = MediaPlayback.query(c, remote);
            if (tracks.hdr) throw new IOException(S.MOVIE_HDR);
            runOnUiThread(() -> {
                List<MediaPlayback.Track> audio = new ArrayList<>(), subtitles = new ArrayList<>();
                for (MediaPlayback.Track t : tracks.tracks) (t.kind == 1 ? audio : subtitles).add(t);
                int[] chosenAudio = {audio.isEmpty() ? -2 : audio.get(0).index}, chosenSub = {-1};
                for (MediaPlayback.Track t : subtitles) if (t.codec.equals("ass") && chinese(t)) { chosenSub[0] = t.index; break; }
                if (audio.size() <= 1 && subtitles.isEmpty()) { playAdapted(c, e, remote, chosenAudio[0], -1); return; }
                Sheet sheet = new Sheet(this, Ui.baseName(e.name), S.MOVIE_TRACKS_SUBTITLE);
                if (audio.size() > 1) {
                    sheet.section(S.MOVIE_AUDIO);
                    List<ImageView> checks = new ArrayList<>();
                    for (int i = 0; i < audio.size(); i++) {
                        MediaPlayback.Track t = audio.get(i);
                        checks.add(choiceRow(sheet, trackName(t, i + 1), null, t.index == chosenAudio[0], checks, () -> chosenAudio[0] = t.index));
                    }
                }
                if (!subtitles.isEmpty()) {
                    sheet.section(S.MOVIE_SUBTITLE);
                    List<ImageView> checks = new ArrayList<>();
                    checks.add(choiceRow(sheet, S.MOVIE_SUBTITLE_OFF, null, chosenSub[0] == -1, checks, () -> chosenSub[0] = -1));
                    for (int i = 0; i < subtitles.size(); i++) {
                        MediaPlayback.Track t = subtitles.get(i);
                        if (t.codec.equals("ass")) checks.add(choiceRow(sheet, trackName(t, i + 1), null, t.index == chosenSub[0], checks, () -> chosenSub[0] = t.index));
                        else sheet.unsupported(-1, trackName(t, i + 1), S.MOVIE_SUBTITLE_ASS_ONLY);
                    }
                }
                sheet.buttons(S.CANCEL, S.MOVIE_PLAY, Ui.PRIMARY, () -> playAdapted(c, e, remote, chosenAudio[0], chosenSub[0])).show();
            });
        });
    }
    /** A single-choice row inside a group that stays open (the sheet's own options dismiss on tap). */
    ImageView choiceRow(Sheet sheet, String title, String detail, boolean checked, List<ImageView> group, Runnable pick) {
        ImageView check = new ImageView(this);
        check.setImageDrawable(new Glyph(Glyph.CHECK, Theme.INK));
        check.setVisibility(checked ? View.VISIBLE : View.INVISIBLE);
        View row = sheet.row(-1, title, detail, Theme.INK, check);
        Ui.role(row, RadioButton.class);
        row.setSelected(checked);
        row.setOnClickListener(v -> {
            for (ImageView other : group) { other.setVisibility(View.INVISIBLE); ((View) other.getParent()).setSelected(false); }
            check.setVisibility(View.VISIBLE); row.setSelected(true); pick.run();
        });
        return check;
    }
    /** Phone screen in landscape, within the adaptation bounds; the computer keeps the aspect ratio. */
    int[] adaptedSize() {
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(m);
        int longSide = Math.max(m.widthPixels, m.heightPixels), shortSide = Math.min(m.widthPixels, m.heightPixels);
        return new int[]{Math.max(320, Math.min(2560, longSide)), Math.max(180, Math.min(2160, shortSide))};
    }
    void playAdapted(Remote.Credentials c, Remote.Entry e, String remote, int audio, int subtitle) {
        int[] size = adaptedSize();
        status(S.MOVIE_PREPARING);
        job(() -> {
            MediaPlayback.closeAll(); // one movie at a time; a previous adapted stream stops here
            MediaPlayback.Adapted adapted = MediaPlayback.openAdapted(this, c, remote, size[0], size[1], audio, subtitle);
            runOnUiThread(() -> {
                try {
                    Intent intent = new Intent(Intent.ACTION_VIEW).setDataAndType(adapted.uri, "application/vnd.apple.mpegurl");
                    startActivity(Intent.createChooser(intent, S.MOVIE_CHOOSER));
                    status(S.MOVIE_ADAPTED_STARTED);
                } catch (ActivityNotFoundException none) {
                    adapted.close();
                    status(S.MOVIE_NO_PLAYER);
                }
            });
        });
    }
    /** choice 0: stream the original through a read-only URI; 1: download, verify, then open; 2: queue a resumable download. */
    void openRemote(Remote.Entry e, String remote, int choice) {
        if(credentials==null){status(S.NEED_DEVICE);return;}
        String name=e.name.substring(e.name.lastIndexOf('/')+1);
        if(choice==2){transfers.add(remote,name,false,0,"");ui.select(AppShell.TRANSFERS);return;}
        cancelled.set(false);
        Remote.Credentials c = credentials;
        if(choice==0)status(S.OPENING);
        job(() -> {
            if (choice == 0) {
                try (Remote r = new Remote(c, 1, 2)) {
                    Remote.Info info = r.stat(remote, false);
                    Uri uri =
                        FilesProvider.share(new FilesProvider.Source(c, remote, info));
                    runOnUiThread(() -> open(uri, name));
                }
            } else {
                String key = Remote.hex(MessageDigest.getInstance("SHA-256").digest(
                    (c.host + c.pin + remote)
                        .getBytes(java.nio.charset.StandardCharsets.UTF_8)));
                File dir = new File(getFilesDir(), "received/" + key);
                if (!dir.mkdirs() && !dir.isDirectory())
                    throw new IOException("缓存目录不可用");
                File dest = new File(dir, name);
                if (!dest.getCanonicalFile().getParentFile().equals(
                        dir.getCanonicalFile()))
                    throw new IOException("远程名称越界");
                try (Remote r = new Remote(c, 1, 3)) {
                    Remote.Info info = r.stat(remote, false);
                    File partial = new File(dest + ".part");
                    long remaining =
                        dest.exists() ? 0 : Math.max(0, info.size - partial.length());
                    long used;
                    try (var paths = java.nio.file.Files.walk(
                             new File(getFilesDir(), "received").toPath())) {
                        used = paths.filter(java.nio.file.Files::isRegularFile)
                                   .mapToLong(p -> p.toFile().length())
                                   .sum();
                    }
                    if (used + remaining > 16L * 1024 * 1024 * 1024 ||
                        new StatFs(getFilesDir().getPath()).getAvailableBytes() <
                            remaining + 64L * 1024 * 1024)
                        throw new IOException(
                            "下载缓存已达 16 GiB "
                            + "上限或磁盘空间不足，请清理缓存或使用原始媒体读取");
                    r.download(remote, dest, cancelled,
                               (n, t) -> status(String.format(Locale.ROOT, S.DOWNLOADING, name, Ui.size(n), Ui.size(t))));
                }
                status("SHA256 已验证，文件已提交。缓存由应用持有。");
                Uri uri = FilesProvider.share(new FilesProvider.Source(dest));
                runOnUiThread(() -> open(uri, name));
            }
        });
    }
    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if(request==REQUEST_SCAN){if(result==RESULT_OK&&data!=null)pairingLink(data.getStringExtra(ScanActivity.EXTRA_LINK));return;}
        if(request==102&&result==RESULT_OK&&data!=null&&data.getData()!=null){
            Uri tree=data.getData();try{getContentResolver().takePersistableUriPermission(tree,Intent.FLAG_GRANT_READ_URI_PERMISSION);
                String doc=android.provider.DocumentsContract.getTreeDocumentId(tree);Uri rootUri=android.provider.DocumentsContract.buildDocumentUriUsingTree(tree,doc);String name;
                try(android.database.Cursor cursor=getContentResolver().query(rootUri,new String[]{"_display_name"},null,null,null)){if(cursor==null||!cursor.moveToFirst())throw new IOException("无法读取所选目录");name=cursor.getString(0);}TreeUpload.safeName(name);
                String base=path.getText().toString();transfers.add(base.isEmpty()?name:base+"/"+name,name,true,2,tree.toString());
            }catch(Exception e){status("目录授权失败："+e.getMessage());}return;
        }
        if(request!=101||result!=RESULT_OK||data==null)return;
        if(data.getClipData()!=null){int count=data.getClipData().getItemCount();if(count>16){status("一次最多选择 16 个文件");return;}for(int i=0;i<count;i++)stageUpload(data.getClipData().getItemAt(i).getUri());}
        else if(data.getData()!=null)stageUpload(data.getData());
    }
    void stageUpload(Uri uri) {
        String dir = path.getText().toString();
        Remote.Credentials c = credentials;
        cancelled.set(false);
        job(() -> {
            String name = "upload.bin";
            try (android.database.Cursor cursor = getContentResolver().query(
                     uri, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (cursor != null && cursor.moveToFirst())
                    name = cursor.getString(0);
            }
            if (name == null || name.isEmpty() || name.contains("/") || name.contains("\\") ||
                name.equals(".") || name.equals(".."))
                throw new IOException("源文件名称无效");
            boolean durable=false;try{getContentResolver().takePersistableUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION);durable=true;}catch(SecurityException unsupported){}
            if(durable){try(SafSource direct=SafSource.open(getContentResolver(),uri)){if(direct!=null){final String uploadName=name,target=dir.isEmpty()?name:dir+"/"+name;runOnUiThread(()->{if(credentials!=c){status("设备已变化，未开始上传");return;}transfers.add(target,uploadName,false,3,uri.toString());});return;}}}
            File source = new File(transfers.root, "upload-" + UUID.randomUUID());
            boolean queued=false;
            try {
                try (InputStream in = getContentResolver().openInputStream(uri);
                     FileOutputStream out = new FileOutputStream(source)) {
                    if (in == null)
                        throw new IOException("源应用无法读取文件");
                    byte[] buffer = new byte[Remote.CHUNK];
                    int n;
                    long total = 0;
                    while ((n = in.read(buffer)) != -1) {
                        if (cancelled.get())
                            throw new IOException("已取消本地暂存");
                        total += n;
                        if (total > 16L * 1024 * 1024 * 1024 ||
                            new StatFs(getCacheDir().getPath()).getAvailableBytes() < n + 64L * 1024 * 1024)
                            throw new IOException("上传暂存空间不足或超过 16 GiB 上限");
                        out.write(buffer, 0, n);
                    }
                    out.getFD().sync();
                }
                final String uploadName=name;
                final String target=dir.isEmpty()?name:dir+"/"+name;
                runOnUiThread(() -> {if(credentials!=c){source.delete();status("设备已变化，未开始上传");return;}transfers.add(target,uploadName,false,1,source.getAbsolutePath());});
                queued=true;
            } finally {
                if(!queued)source.delete();
            }
        });
    }
    final Runnable taskTick=new Runnable(){public void run(){if(ui!=null)renderTransfers();inputUi.postDelayed(this,1000);}};
    @Override protected void onStart(){super.onStart();inputUi.removeCallbacks(taskTick);inputUi.post(taskTick);if(networkWatcher==null)networkWatcher=new NetworkWatcher(this,()->{if(video!=null)video.networkChanged();if(systemAudio!=null)systemAudio.networkChanged();status("网络已变化，正在按原授权恢复连接");});}
    @Override
    protected void onStop() {
        discoveryCancel.set(true);
        if(desktopTools!=null)desktopTools.cancel();
        if(networkWatcher!=null){networkWatcher.close();networkWatcher=null;}
        inputUi.removeCallbacks(taskTick);
        if(systemAudio!=null) {audioSubscribed=false;systemAudio.stop();audioState(AUDIO_OFF);}
        subscribed.set(false);
        TransferPacer.SHARED.realtime=false;
        controlRequested = false; control.set(false); inputEpoch.incrementAndGet(); closeInput(); controlLabels();
        updateDesktopPresentation();
        super.onStop();
    }
    @Override
    protected void onDestroy() {
        presenceSeq.incrementAndGet();
        closePresence();
        if(desktopTools!=null)desktopTools.close();
        if(systemAudio!=null) systemAudio.close();
        cancelled.set(true);
        if(browser!=null)browser.close();
        if(transfers!=null)transfers.detach(this);
        subscribed.set(false);
        frames.shutdownNow();
        if (video != null)
            video.close();
        if (avcView != null)
            avcView.closeSurface();
        Bitmap pending = latestBitmap.getAndSet(null);
        if (pending != null)
            pending.recycle();
        controlRequested = false; control.set(false); inputEpoch.incrementAndGet(); closeInput();
        work.shutdownNow();
        super.onDestroy();
    }
}
