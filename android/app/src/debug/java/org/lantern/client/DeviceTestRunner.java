package org.lantern.client;

import android.app.*;
import android.content.*;
import android.content.pm.ApplicationInfo;
import android.graphics.*;
import android.graphics.drawable.BitmapDrawable;
import android.net.*;
import android.os.*;
import android.system.Os;
import android.view.*;
import android.widget.Button;
import java.io.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.*;

/** Debug-build-only instrumentation. Credentials are received in memory from the authorized adb UID. */
public final class DeviceTestRunner extends Instrumentation {
    final JSONObject report = new JSONObject();
    int assertions;
    boolean desktopOnly, videoOnly, controlOnly, interactionOnly, audioOnly, filesMediaOnly,toolsOnly,safOnly;
    JSONObject inputFixture;
    String externalApp;
    boolean reliabilityOnly;
    boolean deviceTrustOnly;
    void phase(String name) throws Exception {
        report.put("phase", name);
        android.util.Log.i("LanternDeviceTest", name);
    }
    void check(boolean value, String name) throws Exception {
        assertions++;
        if (!value)
            throw new IOException("Device assertion: " + name);
    }
    static double elapsed(long start) {
        return (System.nanoTime() - start) / 1e9;
    }
    static double percentile(List<Double> values, double p) {
        Collections.sort(values);
        return values.get((int)((values.size() - 1) * p));
    }
    /** Legacy step names map to stable UI identifiers; the expected toggle state is asserted. */
    static String[] target(String text) {
        switch (text) {
        case "订阅桌面": return new String[] {"desktop.subscribe", null};
        case "隐藏画面": return new String[] {"desktop.hide", null};
        case "音频：关": return new String[] {"desktop.audio", "off"};
        case "音频：开": return new String[] {"desktop.audio", "on"};
        case "控制：关": return new String[] {"desktop.control", "off"};
        case "控制：开": return new String[] {"desktop.control", "on"};
        case "全屏": return new String[] {"desktop.fullscreen", null};
        case "横竖屏": return new String[] {"full.rotate", null};
        case "收起": return new String[] {"full.collapse", null};
        case "工具": return new String[] {"full.tools", null};
        default: return new String[] {text, null};
        }
    }
    View find(View v, String text) {
        String[] t = target(text);
        View found = Ui.find(v, t[0]);
        if (found != null && t[1] != null && found.isActivated() != t[1].equals("on"))
            return null;
        return found;
    }
    void click(MainActivity activity, String text) throws Exception {
        runOnMainSync(() -> {
            if(text.equals("配对 / 文件")){activity.ui.select(AppShell.DEVICE);activity.ui.pair.performClick();return;}
            if(text.equals("订阅桌面")||text.equals("隐藏画面")||text.startsWith("音频："))activity.ui.select(AppShell.DESKTOP);
            View b = find(activity.getWindow().getDecorView(), text);
            if (b == null)
                throw new IllegalStateException("Control missing or in unexpected state: " + text);
            b.performClick();
        });
        waitForIdleSync();
    }
    void snapshot(MainActivity a,String name)throws Exception{
        waitForIdleSync();Thread.sleep(300);
        Bitmap image=getUiAutomation().takeScreenshot();
        check(image!=null,"UI screenshot "+name);
        File directory=new File(a.getFilesDir(),"ui-validation");directory.mkdirs();
        try(FileOutputStream out=new FileOutputStream(new File(directory,name+".png"))){image.compress(Bitmap.CompressFormat.PNG,100,out);}finally{image.recycle();}
    }
    void fullscreenChecks(MainActivity activity) throws Exception {
        phase("Fullscreen, rotation, zoom and return to files");
        Remote connection = desktopConnection(activity);
        View parent = (View)activity.screen.getParent();
        click(activity, "全屏");
        Thread.sleep(600);
        boolean[] expanded = {false};
        runOnMainSync(()
                          -> expanded[0] = activity.desktopLayout.fullscreen &&
                                           activity.screen.getHeight() > 1000 &&
                                           activity.list.getVisibility() == View.VISIBLE &&
                                           activity.desktopLayout.page.getVisibility() == View.GONE);
        check(expanded[0], "fullscreen replaces form with desktop");
        check(activity.desktopLayout.controlButton.isShown(),"fullscreen inline control button");
        check(desktopConnection(activity) == connection && activity.subscribed.get(),
              "fullscreen preserves connection");
        click(activity, "横竖屏");
        long deadline = System.nanoTime() + 8_000_000_000L;
        boolean[] landscape = {false};
        while (System.nanoTime() < deadline) {
            runOnMainSync(() -> landscape[0] = activity.screen.getWidth() > activity.screen.getHeight());
            if (landscape[0])
                break;
            Thread.sleep(100);
        }
        check(landscape[0], "real landscape layout");
        check(desktopConnection(activity) == connection && activity.subscribed.get(),
              "rotation preserves connection");
        runOnMainSync(() -> {
            long down = SystemClock.uptimeMillis();
            float x = activity.screen.getWidth() / 2f, y = activity.screen.getHeight() / 2f;
            for (int i = 0; i < 2; i++) {
                android.view.MotionEvent event = android.view.MotionEvent.obtain(
                    down + i * 90, down + i * 90, android.view.MotionEvent.ACTION_DOWN, x, y, 0);
                activity.screen.dispatchTouchEvent(event);
                event.recycle();
                event = android.view.MotionEvent.obtain(down + i * 90, down + i * 90 + 30,
                                                        android.view.MotionEvent.ACTION_UP, x, y, 0);
                activity.screen.dispatchTouchEvent(event);
                event.recycle();
            }
        });
        boolean[] zoomed = {false};
        runOnMainSync(()
                          -> zoomed[0] =
                                 activity.screen.zoom == 1 &&
                                 activity.screen.getScaleType() == android.widget.ImageView.ScaleType.MATRIX);
        check(zoomed[0], "double tap does not zoom");
        pinch(activity);
        runOnMainSync(() -> zoomed[0] = activity.screen.zoom > 1);
        check(zoomed[0], "two finger pinch still zooms");
        if(activity.hardwareDesktop)measureVideo(activity,"fullscreen_landscape_",3000);else measureDraws(activity, "fullscreen_landscape_", 5000);
        click(activity, "横竖屏");
        deadline = System.nanoTime() + 8_000_000_000L;
        boolean[] portrait = {false};
        while (System.nanoTime() < deadline) {
            runOnMainSync(() -> portrait[0] = activity.screen.getHeight() > activity.screen.getWidth());
            if (portrait[0])
                break;
            Thread.sleep(100);
        }
        check(portrait[0], "real portrait layout");
        runOnMainSync(activity::onBackPressed);
        waitForIdleSync();
        boolean[] returned = {false};
        runOnMainSync(() -> {
            returned[0] = !activity.desktopLayout.fullscreen &&
                          activity.desktopLayout.page.getVisibility() == View.VISIBLE;
            activity.screen.zoom = 1;
            activity.screen.updateTransform();
        });
        check(returned[0] && activity.subscribed.get(), "back returns to files and preserves subscription");
        check(activity.screen.getParent() == parent && desktopConnection(activity) == connection,
              "same view and connection after fullscreen");
        runOnMainSync(()->activity.ui.select(AppShell.FILES));Thread.sleep(350);
        check(activity.list.isShown()&&!activity.host.isShown(),"file navigation separate from device form");
        check(desktopConnection(activity)==connection&&activity.subscribed.get(),"file navigation preserves desktop subscription");
        runOnMainSync(()->activity.ui.select(AppShell.DESKTOP));Thread.sleep(350);
        report.put("fullscreen_rotation_zoom_verified", true);
    }
    void inputTap(MainActivity a, float x, float y) throws Exception {
        runOnMainSync(() -> {
            long t = SystemClock.uptimeMillis();
            android.view.MotionEvent e = android.view.MotionEvent.obtain(t,t,android.view.MotionEvent.ACTION_DOWN,x,y,0);
            a.screen.dispatchTouchEvent(e); e.recycle();
            e = android.view.MotionEvent.obtain(t,t+30,android.view.MotionEvent.ACTION_UP,x,y,0);
            a.screen.dispatchTouchEvent(e); e.recycle();
        });
        Thread.sleep(420);
        a.inputs.submit(() -> {}).get(5, java.util.concurrent.TimeUnit.SECONDS);
        Thread.sleep(50);
    }
    void pinch(MainActivity a) {
        runOnMainSync(() -> {
            long t=SystemClock.uptimeMillis(); float cx=a.screen.getWidth()/2f, cy=a.screen.getHeight()/2f;
            MotionEvent first=MotionEvent.obtain(t,t,MotionEvent.ACTION_DOWN,cx-100,cy,0);
            a.screen.dispatchTouchEvent(first); first.recycle();
            MotionEvent.PointerProperties[] properties={new MotionEvent.PointerProperties(),new MotionEvent.PointerProperties()};
            MotionEvent.PointerCoords[] coords={new MotionEvent.PointerCoords(),new MotionEvent.PointerCoords()};
            for (int i=0;i<2;i++) { properties[i].id=i; properties[i].toolType=MotionEvent.TOOL_TYPE_FINGER; coords[i].y=cy; coords[i].pressure=1; coords[i].size=1; }
            for (int step=0;step<=8;step++) {
                coords[0].x=cx-100-step*25; coords[1].x=cx+100+step*25;
                int action=step==0 ? MotionEvent.ACTION_POINTER_DOWN | (1<<MotionEvent.ACTION_POINTER_INDEX_SHIFT) : MotionEvent.ACTION_MOVE;
                MotionEvent e=MotionEvent.obtain(t,t+step*35,action,2,properties,coords,0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
                a.screen.dispatchTouchEvent(e); e.recycle();
            }
            MotionEvent e=MotionEvent.obtain(t,t+320,MotionEvent.ACTION_CANCEL,cx,cy,0); a.screen.dispatchTouchEvent(e); e.recycle();
        });
    }
    float[] fixturePoint(MainActivity a, double x, double y) {
        DesktopViewport v=a.screen.viewport;
        return new float[]{v.left+(float)x*v.imageWidth*v.scale, v.top+(float)y*v.imageHeight*v.scale};
    }
    void dragTouch(MainActivity a, float[] start, float[] end, boolean cancel) throws Exception {
        long down=SystemClock.uptimeMillis();
        for (int step=0;step<=8;step++) {
            final int s=step;
            runOnMainSync(() -> {
                float fraction=s/8f;
                int action=s==0 ? MotionEvent.ACTION_DOWN : s==8 ? (cancel ? MotionEvent.ACTION_CANCEL : MotionEvent.ACTION_UP) : MotionEvent.ACTION_MOVE;
                MotionEvent e=MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,start[0]+(end[0]-start[0])*fraction,start[1]+(end[1]-start[1])*fraction,0);
                a.screen.dispatchTouchEvent(e); e.recycle();
            });
            Thread.sleep(100);
            if (step==4 && !cancel) Thread.sleep(2100); // Heartbeats keep a stationary drag alive beyond the host lease.
        }
        a.inputs.submit(() -> {}).get(5,java.util.concurrent.TimeUnit.SECONDS);
    }
    void interactionChecks(MainActivity a, Remote.Credentials c) throws Exception {
        phase("Physical drag, Windows double click and Android IME");
        waitVideo(a); click(a,"全屏");
        runOnMainSync(() -> a.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE));
        Thread.sleep(800); click(a,"控制：关");
        long deadline=System.nanoTime()+5_000_000_000L;
        while(!a.control.get() && System.nanoTime()<deadline) Thread.sleep(50);
        check(a.control.get(),"control with gesture and IME capability"); click(a,"收起");
        double w=inputFixture.getDouble("width"), h=inputFixture.getDouble("height");
        float[][] positions={null,null};
        runOnMainSync(() -> {
            positions[0]=fixturePoint(a,inputFixture.optDouble("title_x")/w,inputFixture.optDouble("title_y")/h);
            positions[1]=fixturePoint(a,inputFixture.optDouble("title_x")/w+.1,inputFixture.optDouble("title_y")/h+.08);
        });
        long before=a.dragEndsCompleted.get(); dragTouch(a,positions[0],positions[1],false);
        check(a.dragEndsCompleted.get()==before+1,"drag through real gesture recognizer and ACK");
        report.put("expected_window_dx",w*.1); report.put("expected_window_dy",h*.08);
        float[][] doublePoint={null}; runOnMainSync(() -> doublePoint[0]=fixturePoint(a,.85,.82));
        before=a.controlClicksCompleted.get();
        runOnMainSync(() -> {
            long t=SystemClock.uptimeMillis();
            for(int i=0;i<2;i++) {
                MotionEvent e=MotionEvent.obtain(t+i*90,t+i*90,MotionEvent.ACTION_DOWN,doublePoint[0][0],doublePoint[0][1],0); a.screen.dispatchTouchEvent(e); e.recycle();
                e=MotionEvent.obtain(t+i*90,t+i*90+30,MotionEvent.ACTION_UP,doublePoint[0][0],doublePoint[0][1],0); a.screen.dispatchTouchEvent(e); e.recycle();
            }
        });
        Thread.sleep(450); a.inputs.submit(() -> {}).get(5,java.util.concurrent.TimeUnit.SECONDS);
        check(a.controlClicksCompleted.get()==before+1 && a.screen.zoom==1,"double click request without zoom");
        float[][] editPoint={null};
        runOnMainSync(() -> editPoint[0]=fixturePoint(a,inputFixture.optDouble("edit_x")/w+.1,inputFixture.optDouble("edit_y")/h+.08));
        inputTap(a,editPoint[0][0],editPoint[0][1]);
        deadline=System.nanoTime()+5_000_000_000L; boolean[] visible={false};
        while(System.nanoTime()<deadline) {
            runOnMainSync(() -> { WindowInsets insets=a.keyboard.getRootWindowInsets(); visible[0]=a.keyboard.accepting && insets!=null && insets.isVisible(WindowInsets.Type.ime()); });
            if(visible[0]) break; Thread.sleep(100);
        }
        report.put("focus_flags",a.lastFocusFlags);
        report.put("keyboard_accepting",a.keyboard.accepting);
        report.put("keyboard_focused",a.keyboard.hasFocus());
        report.put("keyboard_window_focus",a.keyboard.hasWindowFocus());
        check(visible[0],"native Windows Edit click automatically shows physical Android IME");
        runOnMainSync(() -> {
            android.view.inputmethod.InputConnection connection=a.keyboard.onCreateInputConnection(new android.view.inputmethod.EditorInfo());
            connection.commitText("Lantern ",1);
            connection.setComposingText("zhongwen",1);
            connection.setComposingText("中文",1);
            connection.commitText("中文",1);
            connection.commitText(" 😀x",1);
            connection.deleteSurroundingText(1,0);
        });
        a.inputs.submit(() -> {}).get(5,java.util.concurrent.TimeUnit.SECONDS); Thread.sleep(300);
        check(a.textCommitsCompleted.get()>=3,"IME composition committed without sending candidates");
        report.put("expected_test_text","Lantern 中文 😀");
        runOnMainSync(() -> a.keyboard.close());
        // Connection lease releases on disconnect, and independently on missing heartbeats.
        byte[] begin=Remote.body(o -> {o.writeInt(1);o.writeInt(55705);o.writeInt(53740);o.writeInt(0);});
        Remote disconnected=new Remote(c,8,0); disconnected.call(36,begin); Thread.sleep(150); disconnected.abort(); disconnected.close(); Thread.sleep(750);
        try(Remote lease=new Remote(c,8,0)) {
            lease.call(36,begin); Thread.sleep(2250);
            boolean expired=false;
            try { lease.call(36,Remote.body(o -> {o.writeInt(2);o.writeInt(55705);o.writeInt(53740);o.writeInt(0);})); }
            catch(IOException e) { expired=e.getMessage().contains("lease ended"); }
            check(expired,"host expires missing gesture heartbeat");
        }
        before=a.dragEndsCompleted.get(); dragTouch(a,doublePoint[0],new float[]{doublePoint[0][0]-50,doublePoint[0][1]-30},true);
        check(a.dragEndsCompleted.get()==before+1,"cancel gesture releases button");
        pinch(a); check(a.screen.zoom>1,"two finger zoom while control enabled");
        report.put("android_ime_visible_verified",true);
        report.put("ime_text_verification_method","InputConnection composition callbacks to real SendInput; Windows fixture records final text");
        report.put("gesture_lease_timeout_ms",1800);
        click(a,"工具"); click(a,"控制：开"); runOnMainSync(a::onBackPressed); click(a,"订阅桌面");
    }
    void desktopToolsChecks(MainActivity a,Remote.Credentials c)throws Exception{
        phase("Android foreground clipboard and isolated native host");
        android.content.ClipboardManager manager=(android.content.ClipboardManager)a.getSystemService(Context.CLIPBOARD_SERVICE);final ClipData[] original={null};final boolean[] captured={false};
        String generated="Lantern fixture 剪贴板 😀 "+java.util.UUID.randomUUID();
        try{runOnMainSync(()->{original[0]=manager.getPrimaryClip();captured[0]=true;manager.setPrimaryClip(ClipData.newPlainText("Lantern validation",generated));});Thread.sleep(200);
            runOnMainSync(()->a.desktopTools.push(c));long deadline=System.nanoTime()+8_000_000_000L;boolean copied=false;
            while(System.nanoTime()<deadline){try(Remote r=new Remote(c,8,0)){if(DesktopTools.read(r).value.equals(generated)){copied=true;break;}}Thread.sleep(100);}check(copied,"manual phone text push over WiFi pinned TLS");
            String remoteGenerated=generated+" remote";try(Remote r=new Remote(c,8,0)){DesktopTools.Text before=DesktopTools.read(r);DesktopTools.write(r,before.sequence,remoteGenerated);}
            runOnMainSync(()->a.desktopTools.pull(c));deadline=System.nanoTime()+8_000_000_000L;boolean[] pulled={false};
            while(System.nanoTime()<deadline){runOnMainSync(()->{ClipData clip=manager.getPrimaryClip();pulled[0]=clip!=null&&clip.getItemCount()==1&&remoteGenerated.contentEquals(clip.getItemAt(0).getText());});if(pulled[0])break;Thread.sleep(100);}check(pulled[0],"manual remote text pull into real Android clipboard");
            try(Remote r=new Remote(c,4,0)){DataInputStream in=Remote.reader(r.call(20,new byte[0]));check(in.readInt()>0,"monitor enumeration independent of desktop stream");}
            java.util.List<Discovery.Device> found=Discovery.scan(new AtomicBoolean(),c.host);boolean discovered=false;for(Discovery.Device d:found)if(d.host.equals(c.host)&&d.port==c.port&&d.pin.equals(c.pin))discovered=true;check(discovered,"physical WiFi UDP discovery matches pinned TLS host");
            check(!a.subscribed.get()&&a.video.desired==null,"desktop tools require no frame subscription");
            runOnMainSync(()->a.selectMonitor(0));check(a.selectedMonitor==0&&!a.control.get(),"monitor switch resets control before new image");
            runOnMainSync(a::showDesktopTools);waitForIdleSync();getUiAutomation().performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK);waitForIdleSync();
            report.put("actual_second_monitor_tested",false);report.put("native_windows_clipboard_isolated_from_user",true);report.put("user_clipboard_contents_logged_or_transmitted",false);
        }finally{runOnMainSync(()->{a.desktopTools.cancel();if(captured[0]){ClipData now=manager.getPrimaryClip();CharSequence value=now!=null&&now.getItemCount()==1?now.getItemAt(0).getText():null;if(value!=null&&(value.toString().equals(generated)||value.toString().equals(generated+" remote"))){if(original[0]!=null)manager.setPrimaryClip(original[0]);else manager.clearPrimaryClip();}}});}
    }
    void audioChecks(MainActivity a, Remote.Credentials c) throws Exception {
        phase("Independent real WASAPI loopback and AudioTrack playback");
        check(!a.subscribed.get() && a.video.desired==null,"audio test starts without desktop subscription");
        a.timeline.synchronize=true;
        a.systemAudio.measureSamples=true; click(a,"音频：关");
        long deadline=System.nanoTime()+15_000_000_000L;
        while(a.systemAudio.sampleRate==0 && a.systemAudio.failure.isEmpty() && System.nanoTime()<deadline) Thread.sleep(50);
        check(a.systemAudio.sampleRate>0 && a.systemAudio.failure.isEmpty(),"real WASAPI and AudioTrack initialized: "+a.systemAudio.failure);
        Thread.sleep(1800);
        double baselinePower=a.systemAudio.analyzedPackets==0 ? 0 : a.systemAudio.tonePowerSum/a.systemAudio.analyzedPackets;
        File marker=new File(getTargetContext().getFilesDir(),"audio-test-marker.bin");
        try {
            try(FileOutputStream output=new FileOutputStream(marker)) { output.write("LanternAudio".getBytes(java.nio.charset.StandardCharsets.UTF_8)); }
            try(Remote files=new Remote(c,3,3)) { files.upload(marker,"tone-start.bin",new AtomicBoolean(),(n,total)->{}); }
        } finally {marker.delete();}
        Thread.sleep(1500);
        long wire=a.systemAudio.wireBytes.get();
        long samples=a.systemAudio.sampleCount.get(),squares=a.systemAudio.squaredSamples.get(),pcm=a.systemAudio.pcmBytes.get(),played=a.systemAudio.playedFrames;
        long analyzed=a.systemAudio.analyzedPackets; double power=a.systemAudio.tonePowerSum;
        int underruns=a.systemAudio.underruns; long started=System.nanoTime();
        Thread.sleep(4000);
        double seconds=elapsed(started);
        long count=a.systemAudio.sampleCount.get()-samples;
        double tonePower=(a.systemAudio.tonePowerSum-power)/Math.max(1,a.systemAudio.analyzedPackets-analyzed);
        check(count>0 && a.systemAudio.nonZeroSamples.get()>10000,"real nonzero loopback samples");
        check(tonePower>100 && tonePower>baselinePower*1.2,"generated 997Hz tone detected over baseline");
        check(a.systemAudio.playedFrames>played,"AudioTrack driver timestamp frames advance");
        check(!a.subscribed.get() && a.video.desired==null && a.screen.generation==0,"audio playback remains independent of desktop capture");
        report.put("sample_rate",a.systemAudio.sampleRate); report.put("channels",2); report.put("encoding",a.systemAudio.encoding);
        report.put("encoded_payload_Mbps",(a.systemAudio.wireBytes.get()-wire)*8/seconds/1e6);
        report.put("decoded_pcm_Mbps",(a.systemAudio.pcmBytes.get()-pcm)*8/seconds/1e6);
        report.put("audio_track_performance_mode",a.systemAudio.performanceMode);
        report.put("steady_underruns",a.systemAudio.underruns-underruns);
        report.put("actual_driver_played_frames_delta",a.systemAudio.playedFrames-played);
        report.put("generated_tone_mean_power",tonePower); report.put("baseline_tone_mean_power",baselinePower);
        report.put("pcm_rms_normalized",Math.sqrt((a.systemAudio.squaredSamples.get()-squares)/(double)count)/32768);
        phase("System audio, H264 desktop and file transfer in parallel");
        runOnMainSync(() -> a.hardwareDesktop=true); click(a,"订阅桌面"); waitVideo(a);
        long audioBefore=a.systemAudio.pcmBytes.get(),videoBefore=a.video.received.get(),parallelStart=System.nanoTime(),textureBefore=a.avcView.updateCount();
        File target=new File(getTargetContext().getFilesDir(),"audio-parallel-test.bin");
        try(Remote files=new Remote(c,1,3)) {
            Remote.Info info=files.stat("large.bin",true); files.download("large.bin",target,new AtomicBoolean(),(n,total)->{});
            check(Remote.hash(target).equals(info.hash),"parallel 32MiB download SHA256");
        } finally {target.delete();}
        check(a.systemAudio.pcmBytes.get()>audioBefore && a.video.received.get()>videoBefore && a.systemAudio.failure.isEmpty(),"audio and actual H264 continue during file transfer");
        check(a.avcView.updateCount()>textureBefore+10,"actual TextureView continues presenting during transfer");
        report.put("parallel_texture_updates",a.avcView.updateCount()-textureBefore);
        report.put("parallel_download_s",elapsed(parallelStart));
        report.put("parallel_steady_underruns",a.systemAudio.underruns-underruns);
        measureVideo(a,"av_parallel_",3000);report.put("clock_late_drops",a.timeline.dropped);report.put("clock_budget_fallbacks",a.timeline.fallbacks);
        long[] clockSamples=a.timeline.samples();List<Double> skews=new ArrayList<>();for(long value:clockSamples)skews.add(Math.abs(value)/1000.0);
        check(skews.size()>=10,"shared hardware audio clock observations");report.put("software_av_clock_samples",skews.size());report.put("software_av_abs_skew_ms_p50",percentile(skews,.5));report.put("software_av_abs_skew_ms_p95",percentile(skews,.95));report.put("optical_acoustic_av_sync_measured",false);
        check(a.video.desired!=null&&a.video.desired.absolute,"desktop clock-v2 negotiated");
        fullscreenChecks(a);
        click(a,"订阅桌面");
        long stopStart=System.nanoTime(); click(a,"音频：开");
        deadline=System.nanoTime()+2_000_000_000L;
        while(a.systemAudio.active!=null && System.nanoTime()<deadline) Thread.sleep(10);
        check(!a.audioSubscribed && a.systemAudio.active==null && a.systemAudio.desired==null,"audio stop releases TLS and AudioTrack worker");
        report.put("audio_stop_ms",elapsed(stopStart)*1000);
        runOnMainSync(() -> {a.toggleAudio();a.toggleAudio();a.toggleAudio();});
        deadline=System.nanoTime()+8_000_000_000L;
        while((a.systemAudio.desired==null || a.systemAudio.readyRequest!=a.systemAudio.desired) && System.nanoTime()<deadline) Thread.sleep(50);
        check(a.audioSubscribed && a.systemAudio.desired!=null && a.systemAudio.readyRequest==a.systemAudio.desired,"rapid audio off/on retains new subscription generation");
        runOnMainSync(a::toggleAudio);
        report.put("system_audio_verified",true); report.put("acoustic_latency_measured",false); report.put("av_sync_measured",false);
    }
    void controlChecks(MainActivity a) throws Exception {
        phase("Fullscreen controls and physical Windows click targets");
        waitVideo(a); click(a,"全屏");
        runOnMainSync(() -> a.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE));
        Thread.sleep(1000);
        click(a,"收起"); click(a,"工具");
        View[] fullButton = {null};
        runOnMainSync(() -> fullButton[0]=find(a.desktopLayout.toolbar,"控制：关"));
        check(fullButton[0]!=null && a.desktopLayout.fullscreen,"control available inside fullscreen tools");
        click(a,"控制：关");
        long deadline=System.nanoTime()+5_000_000_000L;
        while (!a.control.get() && System.nanoTime()<deadline) Thread.sleep(50);
        check(a.control.get() && a.desktopLayout.fullscreen,"full toolbar grants control without exit");
        click(a,"收起");
        JSONArray expected=new JSONArray();
        for (int scenario=0;scenario<5;scenario++) {
            final int step=scenario;
            if (step==3) {
                runOnMainSync(() -> a.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT));
                Thread.sleep(800);
            }
            if (step==4) {
                runOnMainSync(() -> { a.hardwareDesktop=false; a.desktopProfile=MainActivity.PROFILES[1]; a.updateDesktopPresentation(); });
                Thread.sleep(800);
            }
            float[][] taps={null};
            final double[] expectedFraction={.5,.5};
            runOnMainSync(() -> {
                a.screen.zoom=step==2 ? 2 : 1;
                float vw=a.screen.getWidth(),vh=a.screen.getHeight();
                float drawW=Math.min(vw,vh*16/9f),drawH=drawW*9/16f;
                a.screen.panX=step==2 ? drawW/4 : 0;
                a.screen.panY=step==2 ? drawH/4 : 0;
                a.screen.updateTransform();
                float x=vw/2,y=vh/2;
                if(step==1) { x=(vw-drawW)/2+drawW/4; y=(vh-drawH)/2+drawH/4; expectedFraction[0]=expectedFraction[1]=.25; }
                if(step==2) expectedFraction[0]=expectedFraction[1]=.375;
                taps[0]=new float[]{x,y};
            });
            long before=a.controlClicksCompleted.get();
            inputTap(a,taps[0][0],taps[0][1]);
            check(a.controlClicksCompleted.get()==before+1,"real control acknowledgment " + step);
            expected.put(new JSONObject().put("label",new String[]{"h264_landscape_center","h264_landscape_quarter","h264_zoom_pan_center","h264_portrait_center","jpeg_portrait_center"}[step])
                .put("fraction_x",expectedFraction[0]).put("fraction_y",expectedFraction[1]));
        }
        long before=a.controlClicksCompleted.get();
        inputTap(a,10,10);
        check(a.controlClicksCompleted.get()==before,"black border produces no remote click");
        click(a,"工具"); click(a,"控制：开");
        check(!a.control.get() && a.desktopLayout.fullscreen,"full toolbar disables control without exit");
        inputTap(a,a.screen.getWidth()/2f,a.screen.getHeight()/2f);
        check(a.controlClicksCompleted.get()==before,"disabled control produces no remote click");
        runOnMainSync(a::onBackPressed);
        click(a,"订阅桌面");
        report.put("expected_click_targets",expected);
        report.put("shared_viewport_control_verified",true);
        report.put("fullscreen_control_toggle_verified",true);
    }
    Remote desktopConnection(MainActivity a) {
        return a.hardwareDesktop ? a.video.active : a.desktop;
    }
    void waitVideo(MainActivity a) throws Exception {
        long deadline = System.nanoTime() + 15_000_000_000L;
        while (System.nanoTime() < deadline) {
            if (!a.hardwareDesktop || !a.video.failure.isEmpty())
                throw new IOException("H264 fallback: " + a.video.failure);
            if (a.video.renderedTimes().length >= 10)
                return;
            Thread.sleep(100);
        }
        throw new IOException("H264 first rendered frames timeout; packets=" + a.video.received.get() +
                              "; decoder=" + a.video.decoderName+"; videoHost="+a.timeline.lastVideoHost+"; audioHost="+a.timeline.lastAudioHost+"; deltaUs="+a.timeline.lastDeltaUs+"; drops="+a.timeline.dropped);
    }
    void measureVideo(MainActivity a, String prefix, long duration) throws Exception {
        long start = System.nanoTime(), count, encoded = a.video.encodedBytes.get();
        long surfaces = a.avcView.updateCount(), packets = a.video.received.get(), repeated = a.video.repeated.get();
        synchronized (a.video) {
            count = a.video.rendered;
        }
        Thread.sleep(duration);
        long finish = System.nanoTime(), after;
        synchronized (a.video) {
            after = a.video.rendered;
        }
        List<Double> intervals = new ArrayList<>();
        long[] stamps = a.video.renderedTimes();
        int captured = (int)Math.min(after - count, stamps.length);
        for (int i = Math.max(1, stamps.length - captured + 1); i < stamps.length; i++)
            if (stamps[i] > stamps[i - 1])
                intervals.add((stamps[i] - stamps[i - 1]) / 1e6);
        report.put(prefix + "driver_render_time_first", stamps.length == 0 ? 0 : stamps[0]);
        report.put(prefix + "driver_render_time_last", stamps.length == 0 ? 0 : stamps[stamps.length - 1]);
        report.put(prefix + "rendered_before", count);
        report.put(prefix + "rendered_after", after);
        report.put(prefix + "interval_samples", intervals.size());
        report.put(prefix + "h264_active", a.hardwareDesktop);
        report.put(prefix + "h264_failure", a.video.failure);
        check(after > count && !intervals.isEmpty() && a.hardwareDesktop, "H264 rendered progress");
        report.put(prefix + "codec_render_fps", (after - count) * 1e9 / (finish - start));
        report.put(prefix + "codec_render_interval_ms_p95", percentile(intervals, .95));
        report.put(prefix + "encoded_Mbps", (a.video.encodedBytes.get() - encoded) * 8e3 / (finish - start));
        report.put(prefix + "window_s", (finish - start) / 1e9);
        report.put(prefix + "received_frame_fps", (a.video.received.get() - packets) * 1e9 / (finish - start));
        report.put(prefix + "repeated_capture_frames", a.video.repeated.get() - repeated);
        report.put(prefix + "texture_update_fps", (a.avcView.updateCount() - surfaces) * 1e9 / (finish - start));
        List<Double> updates = new ArrayList<>();
        long[] updateStamps = a.avcView.updateStamps();
        for (int i = 1; i < updateStamps.length; i++) if (updateStamps[i] >= start && updateStamps[i] <= finish)
            updates.add((updateStamps[i] - updateStamps[i - 1]) / 1e6);
        check(!updates.isEmpty(), "actual TextureView image updates");
        report.put(prefix + "texture_update_interval_ms_p95", percentile(updates, .95));
    }
    void videoChecks(MainActivity a, Remote.Credentials c) throws Exception {
        phase("H264 hardware decoder and actual rendered frames");
        waitVideo(a);
        check(a.video.hardwareDecoder, "MediaCodec isHardwareAccelerated");
        report.put("decoder", a.video.decoderName);
        report.put("hardware_decoder", a.video.hardwareDecoder);
        report.put("width", a.video.width);
        report.put("height", a.video.height);
        report.put("first_render_pipeline_ms", a.video.firstFrameMs());
        measureVideo(a, "", 10000);
        fullscreenChecks(a);
        phase("H264 and verified file download in parallel");
        long start = System.nanoTime(), before;
        synchronized (a.video) {
            before = a.video.rendered;
        }
        File folder = new File(getTargetContext().getFilesDir(), "received/video-test-" + UUID.randomUUID());
        if (!folder.mkdirs())
            throw new IOException("Video test cache directory");
        File target = new File(folder, "large.bin");
        try (Remote files = new Remote(c, 1, 3)) {
            Remote.Info source = files.stat("large.bin", true);
            files.download("large.bin", target, new AtomicBoolean(), (n, total) -> {});
            check(Remote.hash(target).equals(source.hash), "parallel file sha256");
        } finally {
            target.delete();
            folder.delete();
        }
        long after;
        synchronized (a.video) {
            after = a.video.rendered;
        }
        double seconds = elapsed(start);
        check(after > before && a.hardwareDesktop, "H264 continues during file transfer");
        report.put("parallel_download_s", seconds);
        report.put("parallel_codec_render_fps", (after - before) / seconds);
        long incoming = a.video.received.get();
        Remote active = a.video.active;
        click(a, "隐藏画面");
        Thread.sleep(1000);
        check(a.subscribed.get() && a.video.received.get() > incoming && a.video.active == active,
              "hidden H264 continues reception");
        click(a, "隐藏画面");
        waitVideo(a);
        click(a, "订阅桌面");
        Thread.sleep(300);
        check(!a.subscribed.get() && a.video.desired == null && a.video.active == null,
              "H264 stop closes stream");
        report.put("h264_desktop_verified", true);
        report.put("render_count_is_optical_latency", false);
    }
    void awaitTask(TransferQueue.Task task,String state,long timeout)throws Exception{long end=System.nanoTime()+timeout*1_000_000L;while(!task.state.equals(state)&&System.nanoTime()<end)Thread.sleep(20);check(task.state.equals(state),"task "+state+": "+task.state+" / "+task.error);}
    TransferQueue.Task latest(TransferQueue queue){synchronized(queue){return queue.tasks.get(queue.tasks.size()-1);}}
    void awaitBytes(TransferQueue.Task task,long bytes)throws Exception{long end=System.nanoTime()+12_000_000_000L;while(task.current<bytes&&System.nanoTime()<end)Thread.sleep(20);check(task.current>=bytes,"actual partial bytes");}
    void reliabilityChecks(MainActivity initial,Remote.Credentials c)throws Exception{
        phase("Background owner survives destroyed Activity");
        runOnMainSync(()->initial.transfers.close());
        TransferQueue q=new TransferQueue(getTargetContext(),new File(getTargetContext().getFilesDir(),"transfers/validation-"+UUID.randomUUID()));q.attach(initial);q.credentials=c;
        runOnMainSync(()->initial.transfers=q);TransferPacer.SHARED.userLimit=1024*1024;
        runOnMainSync(()->q.add("large.bin","background.bin",false,0,""));TransferQueue.Task download=latest(q);awaitBytes(download,2*1024*1024);
        long deadline=System.nanoTime()+5_000_000_000L;while(TransferService.active==null&&System.nanoTime()<deadline)Thread.sleep(20);
        check(TransferService.active!=null,"real dataSync foreground service");long before=download.current;
        runOnMainSync(initial::finish);deadline=System.nanoTime()+5_000_000_000L;while(!initial.isDestroyed()&&System.nanoTime()<deadline)Thread.sleep(20);check(initial.isDestroyed(),"actual Activity destroyed");awaitBytes(download,before+2*1024*1024);
        report.put("background_activity_destroyed_verified",true);report.put("background_progress_bytes",download.current-before);
        phase("Owned host shutdown and restart during background download");
        File signal=new File(getTargetContext().getFilesDir(),"reliability-restart.request");java.nio.file.Files.write(signal.toPath(),new byte[]{1});
        awaitTask(download,"等待网络",12_000);check(new File(q.root,download.id+"/background.bin.part").length()>0,"restart preserves actual partial");
        TransferPacer.SHARED.userLimit=8*1024*1024;awaitTask(download,"完成",40_000);
        try(Remote r=new Remote(c,1,0)){check(Remote.hash(new File(download.local)).equals(r.stat("large.bin",true).hash),"automatic restart resume SHA256");}
        report.put("host_restart_auto_download_verified",true);
        Intent launch=new Intent(getTargetContext(),MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        MainActivity a=(MainActivity)startActivitySync(launch);validationActivity=a;waitForIdleSync();
        check(a.transfers==q,"reopened Activity attaches same queue");runOnMainSync(()->{a.credentials=c;a.ui.connected(c.host);a.ui.select(AppShell.TRANSFERS);});
        phase("Automatic upload resume, immutable conflict and idempotent completion");
        TransferPacer.SHARED.userLimit=1024*1024;
        runOnMainSync(()->q.add("background-upload.bin","background-upload.bin",false,1,download.local));TransferQueue.Task upload=latest(q);awaitBytes(upload,1024*1024);
        long transferred=upload.current;q.networkChanged();awaitTask(upload,"等待网络",5000);TransferPacer.SHARED.userLimit=8*1024*1024;awaitTask(upload,"完成",35_000);
        try(Remote r=new Remote(c,1,0)){check(r.stat(upload.path,true).hash.equals(Remote.hash(new File(download.local))),"auto resumed upload SHA256");}
        check(upload.current>=transferred,"resumed upload reaches final offset");
        runOnMainSync(()->q.add("background-upload.bin","duplicate-upload.bin",false,1,download.local));TransferQueue.Task duplicate=latest(q);awaitTask(duplicate,"完成",10_000);check(duplicate.retries==0,"existing identical content reconciled without overwrite");
        String originalHash;try(Remote r=new Remote(c,1,0)){originalHash=r.stat("readme.txt",true).hash;}
        runOnMainSync(()->q.add("readme.txt","readme.txt",false,1,download.local));TransferQueue.Task conflict=latest(q);awaitTask(conflict,"等待处理",8000);check(conflict.errorCode==4&&conflict.retries==0,"conflict requires decision without automatic retry");
        try(Remote r=new Remote(c,1,0)){check(originalHash.equals(r.stat("readme.txt",true).hash),"conflicting original preserved");}
        runOnMainSync(()->{try{q.renameConflict(conflict,"renamed-upload.bin");}catch(Exception e){throw new RuntimeException(e);}});awaitTask(conflict,"完成",15_000);
        try(Remote r=new Remote(c,1,0)){check(r.stat("renamed-upload.bin",true).hash.equals(Remote.hash(new File(download.local))),"rename conflict new destination verified");check(originalHash.equals(r.stat("readme.txt",true).hash),"original remains after resolution");}
        report.put("auto_upload_resume_verified",true);report.put("conflict_original_preserved_verified",true);report.put("idempotent_upload_verified",true);
        phase("Terminal authorization, explicit pause and platform timeout");
        Remote.Credentials rejected=new Remote.Credentials(c.host,c.port,c.pin,"invalid-test-token");q.setCredentials(rejected);
        runOnMainSync(()->q.add("large.bin","denied.bin",false,0,""));TransferQueue.Task denied=latest(q);awaitTask(denied,"失败",8000);check(denied.errorCode==2&&denied.retries==0,"authorization denial never retries");q.setCredentials(c);
        TransferPacer.SHARED.userLimit=1024*1024;
        runOnMainSync(()->q.add("large.bin","pause.bin",false,0,""));TransferQueue.Task pause=latest(q);awaitBytes(pause,1024*1024);q.networkChanged();awaitTask(pause,"等待网络",5000);q.pause(pause,false);Thread.sleep(1500);check(pause.state.equals("已暂停"),"manual pause suppresses automatic resume");
        runOnMainSync(()->q.resume(pause));awaitBytes(pause,2*1024*1024);TransferService notice=TransferService.active;check(notice!=null,"service active before notification action");notice.notification().actions[0].actionIntent.send();awaitTask(pause,"已暂停",5000);Thread.sleep(200);runOnMainSync(()->q.resume(pause));awaitBytes(pause,3*1024*1024);
        TransferService service=TransferService.active;check(service!=null,"service active before timeout");runOnMainSync(()->service.onTimeout(0,android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC));awaitTask(pause,"已暂停",5000);
        check(new File(q.root,pause.id+"/pause.bin.part").length()>0,"system timeout preserves partial file");Thread.sleep(1200);check(TransferService.active==null,"timeout releases foreground service");
        NetworkProbe.Result measured=NetworkProbe.measure(c,new AtomicBoolean());check(measured.minMillis>0&&measured.maxMillis>=measured.minMillis,"actual five authenticated network samples");report.put("network_rtt_median_ms",measured.medianMillis);report.put("network_handshake_ms",measured.handshakeMillis);
        report.put("platform_timeout_test_method","direct onTimeout callback injection; not a six-hour duration test");report.put("network_change_test_method","callback handler closes a live TLS socket; physical Wi-Fi roaming not measured");report.put("reliability_verified",true);
        phase("H264 and audio recover without JPEG downgrade or input replay");
        runOnMainSync(()->{a.hardwareDesktop=true;a.credentials=c;a.toggleDesktop();a.toggleAudio();});waitVideo(a);
        deadline=System.nanoTime()+10_000_000_000L;while((a.systemAudio.active==null||a.systemAudio.readyRequest!=a.systemAudio.desired)&&System.nanoTime()<deadline)Thread.sleep(20);
        check(a.systemAudio.active!=null&&a.audioSubscribed,"audio subscribed before disruption");Remote oldVideo=a.video.active,oldAudio=a.systemAudio.active;
        long recovery=System.nanoTime();a.video.networkChanged();a.systemAudio.networkChanged();
        deadline=System.nanoTime()+15_000_000_000L;
        while(System.nanoTime()<deadline){if(a.video.active!=null&&a.video.active!=oldVideo&&a.video.rendered>3&&a.systemAudio.active!=null&&a.systemAudio.active!=oldAudio&&a.systemAudio.readyRequest==a.systemAudio.desired&&a.systemAudio.packets.get()>1)break;Thread.sleep(20);}
        check(a.video.active!=null&&a.video.active!=oldVideo&&a.video.rendered>3&&a.hardwareDesktop&&a.subscribed.get(),"H264 auto reconnect retains hardware mode and produces frames");
        check(a.systemAudio.active!=null&&a.systemAudio.active!=oldAudio&&a.systemAudio.readyRequest==a.systemAudio.desired&&a.audioSubscribed,"audio pipeline reacquired after socket loss");
        check(!a.control.get(),"remote input disabled during recovery");report.put("av_socket_recovery_ms",elapsed(recovery)*1000);report.put("h264_reconnects",a.video.reconnects.get());report.put("audio_reconnects",a.systemAudio.reconnects.get());
        runOnMainSync(()->{a.video.networkChanged();a.systemAudio.networkChanged();a.toggleDesktop();a.toggleAudio();});Thread.sleep(800);
        check(a.video.desired==null&&a.video.active==null&&a.systemAudio.desired==null&&a.systemAudio.active==null,"unsubscribe cancels pending recovery");
        runOnMainSync(()->{a.credentials=rejected;a.toggleDesktop();a.toggleAudio();});Thread.sleep(1500);
        check(!a.subscribed.get()&&!a.audioSubscribed&&a.hardwareDesktop,"authorization rejection stops subscriptions without JPEG fallback");runOnMainSync(()->a.credentials=c);
        report.put("desktop_audio_auto_recovery_verified",true);report.put("notification_pause_action_verified",true);
    }
    void trustCommand(int phase)throws Exception{
        File command=new File(getTargetContext().getFilesDir(),"device-trust.request"),ack=new File(getTargetContext().getFilesDir(),"device-trust.ack");java.nio.file.Files.write(command.toPath(),new byte[]{(byte)('0'+phase)});
        long deadline=System.nanoTime()+10_000_000_000L;boolean applied=false;
        while(System.nanoTime()<deadline){if(ack.exists()){byte[] result=java.nio.file.Files.readAllBytes(ack.toPath());if(result.length>0&&result[0]=='0'+phase){applied=true;break;}}Thread.sleep(20);}check(applied,"host device command applied "+phase);Thread.sleep(350);
    }
    void deviceTrustChecks(MainActivity a,Remote.Credentials c)throws Exception{
        phase("Android unique device credential and actual write grant");
        try(Remote write=new Remote(c,3,0)){write.mutate(8,"android-device-folder","");check(write.page("",0,"",0,false,"").entries.stream().anyMatch(e->e.name.equals("android-device-folder")),"device write creates folder");}
        trustCommand(1);
        boolean denied=false;try(Remote wrong=new Remote(c,3,0)){}catch(Remote.RemoteError error){denied=error.code==2;}check(denied,"reduced Android device grant denies write");
        try(Remote reader=new Remote(c,1,0)){check(reader.list("",0).size()>=2,"read still permitted after grant reduction");trustCommand(2);boolean ended=false;try{reader.list("",0);}catch(IOException expected){ended=true;}check(ended,"actual Android live TLS session ended");}
        try(Remote resumed=new Remote(c,1,0)){check(resumed.list("",0).size()>=2,"session end retains credential for reconnect");}
        trustCommand(3);denied=false;try(Remote revoked=new Remote(c,0,0)){}catch(Remote.RemoteError error){denied=error.code==2;}check(denied,"revoked device identity rejected even without file scope");
        report.put("unique_android_device_credential_verified",true);report.put("grant_reduction_verified",true);report.put("session_end_verified",true);report.put("device_revocation_verified",true);
    }
    void filesMediaChecks(MainActivity a,Remote.Credentials c)throws Exception {
        phase("New file management protocol and host operations");
        try(Remote r=new Remote(c,3,0)) {
            Remote.Page page=r.page("fixture",0,"",0,false,"");check(page.entries.size()>=3,"sorted fixture page");
            Remote.SearchPage found=r.search(0,"fixture","hit");int hits=found.entries.size();while(!found.done){found=r.search(1,"","");hits+=found.entries.size();}check(hits==1,"recursive search");
            byte[] jpeg=r.thumbnail("fixture/image.bmp");check(BitmapFactory.decodeByteArray(jpeg,0,jpeg.length)!=null,"actual thumbnail decode");
            String id=r.startJob(0,"fixture","copy-fixture");Remote.HostJob job;do{job=r.pollJob(id);Thread.sleep(20);}while(!job.terminal());check(job.state==4,"host directory copy");
            check(r.stat("fixture/nested/hit.txt",true).hash.equals(r.stat("copy-fixture/nested/hit.txt",true).hash),"host copy hash");
            r.mutate(9,"copy-fixture","renamed-fixture");id=r.startJob(1,"renamed-fixture","");do{job=r.pollJob(id);Thread.sleep(20);}while(!job.terminal());check(job.state==4,"host recursive delete");
        }
        phase("Persistent transfer queue, pause resume and directory commit");
        TransferPacer.SHARED.userLimit=1024*1024;
        runOnMainSync(()->a.transfers.add("large.bin","queue-test.bin",false,0,""));
        TransferQueue.Task task=a.transfers.tasks.get(a.transfers.tasks.size()-1);long deadline=System.nanoTime()+15_000_000_000L;
        while(task.current<1024*1024&&System.nanoTime()<deadline)Thread.sleep(50);
        runOnMainSync(()->a.transfers.pause(task,false));while(a.transfers.running!=null&&System.nanoTime()<deadline)Thread.sleep(50);
        check(task.state.equals("已暂停")&&task.current>0,"pause preserves partial bytes");
        check(new File(a.transfers.root,task.id+"/queue-test.bin.part").length()>0,"real partial on disk");
        TransferPacer.SHARED.userLimit=32L*1024*1024;
        runOnMainSync(()->a.transfers.resume(task));deadline=System.nanoTime()+50_000_000_000L;
        while(!task.state.equals("完成")&&!task.state.equals("失败")&&System.nanoTime()<deadline)Thread.sleep(100);
        check(task.state.equals("完成"),"resumed queue completes: "+task.error);
        try(Remote r=new Remote(c,1,0)){check(Remote.hash(new File(task.local)).equals(r.stat("large.bin",true).hash),"queued file SHA256");}
        runOnMainSync(()->a.transfers.add("fixture","fixture",true,0,""));TransferQueue.Task directory=a.transfers.tasks.get(a.transfers.tasks.size()-1);deadline=System.nanoTime()+30_000_000_000L;
        while(!directory.state.equals("完成")&&!directory.state.equals("失败")&&System.nanoTime()<deadline)Thread.sleep(100);
        check(directory.state.equals("完成")&&new File(directory.local,"nested/hit.txt").isFile(),"directory final commit: "+directory.error);
        runOnMainSync(()->a.transfers.close());TransferQueue[] restored={null};runOnMainSync(()->restored[0]=new TransferQueue(a));
        check(restored[0].tasks.stream().anyMatch(t->t.id.equals(task.id)&&t.state.equals("完成")),"journal restores task identity");runOnMainSync(()->{a.transfers=restored[0];});
        phase("SAF directory upload through document provider");
        File saf=new File(a.getFilesDir(),"saf-test-fixture");new File(saf,"nested").mkdirs();java.nio.file.Files.write(new File(saf,"nested/upload.txt").toPath(),"Lantern generated SAF fixture".getBytes("UTF-8"));
        String tree=android.provider.DocumentsContract.buildTreeDocumentUri("org.lantern.client.testdocs","root").toString();
        runOnMainSync(()->a.transfers.add("saf-upload","saf-upload",true,2,tree));TransferQueue.Task upload=a.transfers.tasks.get(a.transfers.tasks.size()-1);deadline=System.nanoTime()+20_000_000_000L;
        while(!upload.state.equals("完成")&&!upload.state.equals("失败")&&System.nanoTime()<deadline)Thread.sleep(50);
        check(upload.state.equals("完成"),"SAF tree upload: "+upload.error);try(Remote r=new Remote(c,1,0)){check(r.stat("saf-upload/nested/upload.txt",true).hash.equals(Remote.hash(new File(saf,"nested/upload.txt"))),"SAF upload hash");}
        phase("Multiple selected documents enter upload queue");
        java.nio.file.Files.write(new File(saf,"batch.txt").toPath(),"Generated second upload".getBytes("UTF-8"));
        Uri first=android.provider.DocumentsContract.buildDocumentUriUsingTree(Uri.parse(tree),"nested/upload.txt"),second=android.provider.DocumentsContract.buildDocumentUriUsingTree(Uri.parse(tree),"batch.txt");
        ClipData clip=ClipData.newRawUri("Test selection",first);clip.addItem(new ClipData.Item(second));Intent selection=new Intent();selection.setClipData(clip);
        runOnMainSync(()->a.onActivityResult(101,Activity.RESULT_OK,selection));deadline=System.nanoTime()+25_000_000_000L;boolean batchDone=false;
        while(System.nanoTime()<deadline){synchronized(a.transfers){batchDone=a.transfers.tasks.stream().filter(t->t.pin.equals(c.pin)&&t.kind==1&&t.state.equals("完成")&&(t.path.equals("upload.txt")||t.path.equals("batch.txt"))).count()==2;}if(batchDone)break;Thread.sleep(50);}
        check(batchDone,"multi selection upload queue completed");try(Remote r=new Remote(c,1,0)){check(r.stat("batch.txt",true).hash.equals(Remote.hash(new File(saf,"batch.txt"))),"batch upload hash");}
        phase("External application URI and original random access");
        Remote.Info media;try(Remote r=new Remote(c,1,2)){media=r.stat("movie.mp4",false);}
        Uri shared=FilesProvider.share(new FilesProvider.Source(c,"movie.mp4",media));
        try(android.os.ParcelFileDescriptor descriptor=getTargetContext().getContentResolver().openFileDescriptor(shared,"r");Remote r=new Remote(c,1,2)){
            check(descriptor!=null,"original media seekable content URI");
            for(long pos:new long[]{0,media.size/2,Math.max(0,media.size-4096)}){byte[] expected=r.read("movie.mp4",media.version,pos,(int)Math.min(4096,media.size-pos));byte[] actual=new byte[expected.length];Os.lseek(descriptor.getFileDescriptor(),pos,android.system.OsConstants.SEEK_SET);int offset=0;while(offset<actual.length){int got=Os.read(descriptor.getFileDescriptor(),actual,offset,actual.length-offset);check(got>0,"proxy file read progress");offset+=got;}check(Arrays.equals(expected,actual),"original URI random read bytes");}
        }
        boolean removed=false;try{getTargetContext().getPackageManager().getActivityInfo(new ComponentName(getTargetContext().getPackageName(),"org.lantern.client.PlayerActivity"),0);}catch(android.content.pm.PackageManager.NameNotFoundException e){removed=true;}
        check(removed,"built-in player activity removed");
        runOnMainSync(()->a.ui.select(AppShell.TRANSFERS));check(a.ui.selected==AppShell.TRANSFERS,"dedicated transfer page");
        snapshot(a,"transfers");
        runOnMainSync(()->a.ui.select(AppShell.FILES));check(a.list.isShown()&&!a.host.isShown(),"files page hides pairing fields");
        snapshot(a,"files");
        runOnMainSync(()->{a.ui.select(AppShell.DEVICE);a.pin.setText("");a.secret.setText("");});snapshot(a,"device");
        runOnMainSync(()->a.ui.select(AppShell.DESKTOP));snapshot(a,"desktop-idle");
        runOnMainSync(()->a.ui.select(AppShell.FILES));
        if(externalApp!=null){
            phase("System chooser and existing phone application");
            runOnMainSync(()->a.open(shared,"movie.mp4"));Thread.sleep(800);
            android.view.accessibility.AccessibilityNodeInfo chooser=getUiAutomation().getRootInActiveWindow();
            check(chooser!=null&&!getTargetContext().getPackageName().contentEquals(chooser.getPackageName()),"system open-with chooser shown");
            getUiAutomation().performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK);Thread.sleep(500);
            ComponentName player=ComponentName.unflattenFromString(externalApp);check(player!=null,"external player component");
            runOnMainSync(()->{Intent video=new Intent(Intent.ACTION_VIEW).setComponent(player).setDataAndType(shared,"video/mp4").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);video.setClipData(ClipData.newRawUri("Generated Lantern fixture",shared));a.startActivity(video);});
            Thread.sleep(2500);snapshot(a,"external-player");
            android.view.accessibility.AccessibilityNodeInfo root=getUiAutomation().getRootInActiveWindow();
            check(root!=null&&player.getPackageName().contentEquals(root.getPackageName()),"existing phone player foreground");
            report.put("external_player_package",player.getPackageName());report.put("external_chooser_verified",true);report.put("external_player_foreground_verified",true);
            getUiAutomation().performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK);Thread.sleep(500);
        }
        report.put("file_media_verified",true);report.put("built_in_player_removed",true);report.put("external_original_uri_verified",true);report.put("4K_playback_measured",false);

    }
    @Override
    public void onCreate(Bundle args) {
        super.onCreate(args);
        desktopOnly = args != null && "true".equals(args.getString("desktop_only"));
        videoOnly = args != null && "true".equals(args.getString("video_only"));
        controlOnly = args != null && "true".equals(args.getString("control_only"));
        interactionOnly = args != null && "true".equals(args.getString("interaction_only"));
        filesMediaOnly = args != null && "true".equals(args.getString("files_media_only"));
        safOnly=args!=null&&"true".equals(args.getString("saf_stream_only"));
        toolsOnly=args!=null&&"true".equals(args.getString("tools_only"));
        audioOnly = args != null && "true".equals(args.getString("audio_only"));
        externalApp=args==null?null:args.getString("external_app");
        reliabilityOnly=args!=null&&"true".equals(args.getString("reliability_only"));
        deviceTrustOnly=args!=null&&"true".equals(args.getString("device_trust_only"));
        try { if(interactionOnly) inputFixture=new JSONObject(new String(android.util.Base64.decode(args.getString("input_fixture"),android.util.Base64.NO_WRAP),java.nio.charset.StandardCharsets.UTF_8)); } catch(JSONException e) { throw new IllegalArgumentException(e); }
        start();
    }
    void measureDraws(MainActivity activity, String prefix, long duration) throws Exception {
        runOnMainSync(() -> activity.screen.resetDrawMetrics());
        long start = System.nanoTime();
        Thread.sleep(duration);
        long[][] stamps = {null};
        runOnMainSync(() -> stamps[0] = activity.screen.drawTimes());
        double seconds = elapsed(start);
        List<Double> intervals = new ArrayList<>();
        for (int i = 1; i < stamps[0].length; i++)
            intervals.add((stamps[0][i] - stamps[0][i - 1]) / 1e6);
        check(stamps[0].length >= 2, "distinct UI image draws " + prefix);
        report.put(prefix + "desktop_ui_unique_draws", stamps[0].length);
        report.put(prefix + "desktop_ui_window_s", seconds);
        report.put(prefix + "desktop_ui_draw_fps", stamps[0].length / seconds);
        report.put(prefix + "desktop_ui_draw_interval_ms_p50", percentile(intervals, .5));
        report.put(prefix + "desktop_ui_draw_interval_ms_p95", percentile(intervals, .95));
    }
    Map<String,?> savedDevice,savedSecret;byte[] savedJournal;boolean journalExisted;
    MainActivity validationActivity;
    void saveUserState(Context c)throws Exception{savedDevice=new HashMap<>(c.getSharedPreferences("device",0).getAll());savedSecret=new HashMap<>(c.getSharedPreferences("secret",0).getAll());File journal=new File(c.getFilesDir(),"transfers/tasks.json");journalExisted=journal.exists();if(journalExisted)savedJournal=java.nio.file.Files.readAllBytes(journal.toPath());}
    void restorePreference(Context c,String name,Map<String,?> values){if(values==null)return;android.content.SharedPreferences.Editor e=c.getSharedPreferences(name,0).edit().clear();for(Map.Entry<String,?> pair:values.entrySet()){Object value=pair.getValue();if(value instanceof String)e.putString(pair.getKey(),(String)value);else if(value instanceof Integer)e.putInt(pair.getKey(),(Integer)value);else if(value instanceof Boolean)e.putBoolean(pair.getKey(),(Boolean)value);else if(value instanceof Long)e.putLong(pair.getKey(),(Long)value);else if(value instanceof Float)e.putFloat(pair.getKey(),(Float)value);}e.commit();}
    void finishValidation(int code,Bundle result){
        try{Context c=getTargetContext();if(validationActivity!=null&&validationActivity.transfers!=null){runOnMainSync(()->validationActivity.transfers.close());long deadline=System.nanoTime()+5_000_000_000L;while(validationActivity.transfers.running!=null&&System.nanoTime()<deadline)Thread.sleep(20);}
            restorePreference(c,"device",savedDevice);restorePreference(c,"secret",savedSecret);
            if(savedDevice!=null){File journal=new File(c.getFilesDir(),"transfers/tasks.json");if(journalExisted){android.util.AtomicFile atomic=new android.util.AtomicFile(journal);FileOutputStream out=atomic.startWrite();out.write(savedJournal);atomic.finishWrite(out);}else journal.delete();}
        }catch(Exception e){result.putString("restore_error",e.getClass().getSimpleName());}
        finish(code,result);
    }
    @Override
    public void onStart() {
        Bundle result = new Bundle();
        try {
            Context context = getTargetContext();
            saveUserState(context);
            if ((context.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0)
                throw new SecurityException("Debug build required");
            report.put("kind", "measured");
            report.put("model", Build.MODEL);
            report.put("android_release", Build.VERSION.RELEASE);
            report.put("sdk", Build.VERSION.SDK_INT);
            report.put("abi", Build.SUPPORTED_ABIS[0]);
            report.put("test_mode", controlOnly ? "fullscreen_control" : videoOnly ? "hardware_desktop" : desktopOnly ? "desktop_profiles" : "full_device_validation");
            MainActivity activity = (MainActivity)startActivitySync(
                new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            runOnMainSync(
                () -> activity.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
            validationActivity=activity;
            Remote.Credentials credentials;
            try (LocalServerSocket server = new LocalServerSocket("lantern-device-validation");
                 LocalSocket peer = server.accept()) {
                int uid = peer.getPeerCredentials().getUid();
                if (uid != 0 && uid != 2000)
                    throw new SecurityException("Authorized adb peer required");
                peer.setSoTimeout(15000);
                DataInputStream in = new DataInputStream(peer.getInputStream());
                if (in.readInt() != 1)
                    throw new IOException("Provisioning version");
                String host = Remote.str(in);
                int port = in.readInt();
                String pin = Remote.str(in), token = Remote.str(in);
                if (host.length() > 256 || !pin.matches("[0-9a-f]{64}") || !token.matches("[0-9a-f]{64}"))
                    throw new IOException("Provisioning limits");
                credentials = new Remote.Credentials(host, port, pin, token);
                report.put("server", host);
                report.put("network_path",
                           host.equals("127.0.0.1") ? "adb_reverse_usb" : "physical_wifi_lan");
                peer.getOutputStream().write(1);
                peer.getOutputStream().flush();
            }
            long began = System.nanoTime();
            if (!desktopOnly && !videoOnly && !controlOnly && !interactionOnly && !audioOnly && !filesMediaOnly && !reliabilityOnly && !deviceTrustOnly && !toolsOnly && !safOnly) {
                phase("TLS and independent file list");
                try (Remote remote = new Remote(credentials, 3, 3)) {
                    report.put("tls_auth_ms", elapsed(began) * 1000);
                    check(!remote.list("", 0).isEmpty(), "independent list");
                    Remote.Info info = remote.stat("large.bin", true);
                    check(info.size == 32L * 1024 * 1024, "source size");
                    File folder =
                        new File(context.getFilesDir(), "received/device-test-" + UUID.randomUUID());
                    if (!folder.mkdirs())
                        throw new IOException("Test directory");
                    File target = new File(folder, "large.bin");
                    AtomicBoolean cancel = new AtomicBoolean();
                    boolean paused = false;
                    phase("Cancel download at 1 MiB");
                    try {
                        remote.download("large.bin", target, cancel, (n, total) -> {
                            if (n >= 1024 * 1024)
                                cancel.set(true);
                        });
                    } catch (IOException expected) {
                        paused = cancel.get();
                    }
                    check(paused && !target.exists(), "cancel preserves partial");
                    cancel.set(false);
                    long[] resumed = {0};
                    began = System.nanoTime();
                    phase("Resume download");
                    remote.download("large.bin", target, cancel, (n, total) -> {
                        if (resumed[0] == 0)
                            resumed[0] = n;
                    });
                    double resumeSeconds = elapsed(began);
                    check(resumed[0] == 1024 * 1024, "download resume offset");
                    check(Remote.hash(target).equals(info.hash), "download sha256");
                    report.put("download_resume_offset", resumed[0]);
                    report.put("download_remaining_31MiB_with_hash_s", resumeSeconds);
                    report.put("download_remaining_MiB_per_s", 31 / resumeSeconds);
                    File complete = new File(folder, "complete.bin");
                    began = System.nanoTime();
                    remote.download("large.bin", complete, new AtomicBoolean(), (n, total) -> {});
                    double downloadSeconds = elapsed(began);
                    report.put("download_32MiB_s", downloadSeconds);
                    report.put("download_MiB_per_s", 32 / downloadSeconds);
                    phase("Cancel and resume upload");
                    String upload = "phone-upload-" + UUID.randomUUID() + ".bin";
                    cancel.set(false);
                    paused = false;
                    try {
                        remote.upload(target, upload, cancel, (n, total) -> {
                            if (n >= 1024 * 1024)
                                cancel.set(true);
                        });
                    } catch (IOException expected) {
                        paused = cancel.get();
                    }
                    check(paused, "upload cancel");
                    cancel.set(false);
                    resumed[0] = 0;
                    began = System.nanoTime();
                    remote.upload(target, upload, cancel, (n, total) -> {
                        if (resumed[0] == 0)
                            resumed[0] = n;
                    });
                    double uploadSeconds = elapsed(began);
                    check(resumed[0] == 1024 * 1024, "upload resume offset");
                    check(remote.stat(upload, true).hash.equals(info.hash), "upload sha256");
                    report.put("upload_resume_offset", resumed[0]);
                    report.put("upload_remaining_31MiB_with_hash_s", uploadSeconds);
                    report.put("upload_remaining_MiB_per_s", 31 / uploadSeconds);
                    try (Remote readOnly = new Remote(credentials, 1, 3)) {
                        boolean denied = false;
                        try {
                            readOnly.call(8, Remote.body(o -> {
                                Remote.str(o, "forbidden");
                                Remote.str(o, "");
                            }));
                        } catch (IOException expected) {
                            denied = true;
                        }
                        check(denied, "read-only write denied");
                    }
                    Remote.Info media = remote.stat("large.bin", false);
                    byte[] expected = remote.read("large.bin", media.version, 731, 65536);
                    phase("Seekable original-media descriptor");
                    Uri uri = FilesProvider.share(new FilesProvider.Source(credentials, "large.bin", media));
                    try (ParcelFileDescriptor fd =
                             context.getContentResolver().openFileDescriptor(uri, "r")) {
                        byte[] actual = new byte[65536];
                        int n = Os.pread(fd.getFileDescriptor(), actual, 0, actual.length, 731);
                        check(n == actual.length && Arrays.equals(expected, actual), "proxy FD random read");
                        report.put("proxy_fd_random_read", true);
                    }
                    boolean denied = false;
                    try (ParcelFileDescriptor ignored =
                             context.getContentResolver().openFileDescriptor(uri, "rw")) {
                    } catch (Exception expectedFailure) {
                        denied = true;
                    }
                    check(denied, "proxy write denied");
                    Uri localUri = FilesProvider.share(new FilesProvider.Source(target));
                    try (ParcelFileDescriptor fd =
                             context.getContentResolver().openFileDescriptor(localUri, "r")) {
                        check(fd.getStatSize() == info.size, "cached content URI");
                    }
                    // File provider intentionally confines saved documents to the received root.
                    target.delete();
                    complete.delete();
                    folder.delete();
                }
            }
            Secrets.save(context, credentials.token);
            check(Secrets.load(context).equals(credentials.token), "Keystore roundtrip");
            check(!context.getSharedPreferences("secret", 0)
                       .getString("cipher", "")
                       .contains(credentials.token),
                  "no plaintext preference");
            runOnMainSync(() -> {
                activity.hardwareDesktop = videoOnly || controlOnly || interactionOnly;
                activity.host.setText(credentials.host+(credentials.port==47443?"":":"+credentials.port));
                activity.pin.setText(credentials.pin);
                activity.secret.setText(credentials.token);
            });
            click(activity, "配对 / 文件");
            phase("Real UI pairing and desktop");
            long deadline = System.nanoTime() + 15_000_000_000L;
            boolean[] listed = {false};
            while (System.nanoTime() < deadline) {
                runOnMainSync(() -> listed[0] = !activity.entries.isEmpty());
                if (listed[0])
                    break;
                Thread.sleep(100);
            }
            if(!listed[0]){
                runOnMainSync(()->{try{report.put("pairing_ui_notice",activity.ui.status.getText().toString());report.put("pairing_ui_state",activity.ui.deviceState.getText().toString());}catch(Exception ignored){}});
                try(Remote probe=new Remote(credentials,1,0)){report.put("pairing_direct_file_probe_entries",probe.list("",0).size());}
            }
            check(listed[0], "actual UI pairing and file list");
            if (!audioOnly && !filesMediaOnly && !reliabilityOnly && !deviceTrustOnly && !toolsOnly && !safOnly) click(activity, "订阅桌面");
            if(safOnly){SafValidation.run(this,activity,credentials);
            }else if(toolsOnly){desktopToolsChecks(activity,credentials);
            }else if(deviceTrustOnly){
                deviceTrustChecks(activity,credentials);
            }else if(reliabilityOnly){
                reliabilityChecks(activity,credentials);
            } else if (filesMediaOnly) {
                filesMediaChecks(activity,credentials);
            } else if (audioOnly) {
                audioChecks(activity,credentials);
            } else if (interactionOnly) {
                interactionChecks(activity,credentials);
            } else if (controlOnly) {
                controlChecks(activity);
            } else if (videoOnly) {
                videoChecks(activity, credentials);
            } else {
                deadline = System.nanoTime() + 15_000_000_000L;
                boolean[] shown = {false};
                while (System.nanoTime() < deadline) {
                    runOnMainSync(() -> shown[0] = activity.screen.getDrawable() instanceof BitmapDrawable);
                    if (shown[0])
                        break;
                    Thread.sleep(100);
                }
                check(shown[0], "actual UI JPEG presentation");
                phase("Measure actual UI image draws without competing desktop requests");
                runOnMainSync(() -> activity.desktopProfile = MainActivity.PROFILES[1]);
                Thread.sleep(300);
                measureDraws(activity, "", 10000);
                report.put("desktop_ui_draw_is_optical_measurement", false);
                if (desktopOnly) {
                    for (int index : new int[] {0, 2}) {
                        MainActivity.DesktopProfile profile = MainActivity.PROFILES[index];
                        String prefix = index == 0 ? "fast_" : "sharp_";
                        runOnMainSync(() -> activity.desktopProfile = profile);
                        Thread.sleep(500);
                        measureDraws(activity, prefix, 5000);
                        int[] width = {0};
                        runOnMainSync(() -> width[0] = activity.screen.imageWidth);
                        check(width[0] > 0 && width[0] <= profile.width, "profile image width");
                        report.put(prefix + "decoded_width", width[0]);
                        report.put(prefix + "requested_quality", profile.quality);
                    }
                    runOnMainSync(() -> activity.desktopProfile = MainActivity.PROFILES[1]);
                    fullscreenChecks(activity);
                }
                try (Remote desktop = new Remote(credentials, 4, 1)) {
                    List<Double> latencies = new ArrayList<>();
                    long bytes = 0;
                    began = System.nanoTime();
                    for (int i = 0; i < 30; i++) {
                        long t = System.nanoTime();
                        byte[] reply = desktop.call(21, Remote.body(o -> {
                            o.writeInt(0);
                            o.writeInt(1280);
                            o.writeInt(65);
                        }));
                        byte[] jpg = Remote.blob(Remote.reader(reply), Remote.MAX);
                        Bitmap b = BitmapFactory.decodeByteArray(jpg, 0, jpg.length);
                        check(b != null && b.getWidth() > 0, "actual JPEG decode");
                        b.recycle();
                        bytes += jpg.length;
                        latencies.add(elapsed(t) * 1000);
                    }
                    double seconds = elapsed(began);
                    report.put("desktop_capture_tls_bitmap_30_fps", 30 / seconds);
                    report.put("desktop_capture_tls_bitmap_ms_p50", percentile(latencies, .5));
                    report.put("desktop_capture_tls_bitmap_ms_p95", percentile(latencies, .95));
                    report.put("desktop_jpeg_bytes", bytes);
                }
                click(activity, "隐藏画面");
                check(activity.subscribed.get() && !activity.display.get(), "hide preserves subscription");
                click(activity, "隐藏画面");
                click(activity, "订阅桌面");
                check(!activity.subscribed.get(), "stop subscription");
            }
            report.put("assertions_passed", assertions);
            report.put("success", true);
            report.put("end_to_end_input_latency_measured", false);
            if(!filesMediaOnly)report.put("4K_playback_measured", false);
            runOnMainSync(
                () -> activity.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
            context.getSharedPreferences("device", 0)
                .edit()
                .putString("host", credentials.host)
                .putString("pin", credentials.pin)
                .apply();
            result.putString("report", report.toString());
            finishValidation(Activity.RESULT_OK, result);
        } catch (Throwable failure) {
            try {
                report.put("success", false);
                report.put("assertions_passed", assertions);
                report.put("error", failure.getClass().getSimpleName() + ": " + failure.getMessage());
                JSONArray stack=new JSONArray();for(StackTraceElement frame:failure.getStackTrace())if(stack.length()<12)stack.put(frame.toString());report.put("failure_stack",stack);
                if(failure.getCause()!=null)report.put("failure_cause",failure.getCause().getClass().getSimpleName()+": "+failure.getCause().getMessage());
            } catch (Exception ignored) {
            }
            result.putString("report", report.toString());
            finishValidation(Activity.RESULT_CANCELED, result);
        }
    }
}
