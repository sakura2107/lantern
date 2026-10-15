package org.lantern.client;

public final class ViewportTest {
    static int checks;
    static void check(boolean value, String text) { checks++; if (!value) throw new AssertionError(text); }
    static void near(int actual, int expected, int tolerance, String text) { check(Math.abs(actual - expected) <= tolerance, text); }
    public static void main(String[] args) {
        DesktopViewport v = new DesktopViewport();
        v.update(1920, 1080, 2780, 1264, 1, 0, 0);
        int[] center = v.pointer(1390, 632);
        near(center[0], 32768, 20, "landscape center X"); near(center[1], 32768, 40, "landscape center Y");
        check(v.pointer(10,632) == null, "landscape left letterbox");
        check(v.pointer(2770,632) == null, "landscape right letterbox");
        v.update(1920,1080,1264,2780,1,0,0);
        center = v.pointer(632,1390);
        near(center[0],32768,20,"portrait center X");near(center[1],32768,40,"portrait center Y");
        check(v.pointer(632,20)==null,"portrait top letterbox");check(v.pointer(632,2760)==null,"portrait bottom letterbox");
        v.update(1920,1080,1920,1080,2,0,0);
        int[] quarter=v.pointer(0,0);
        near(quarter[0],16384,20,"2x crop X");near(quarter[1],16384,30,"2x crop Y");
        v.update(1920,1080,1920,1080,2,300,150);
        center=v.pointer(960,540);
        near(center[0],27662,20,"pan X shifts source");near(center[1],28217,30,"pan Y shifts source");
        v.update(1920,1080,1920,1080,4,100000,-100000);
        near(v.pointer(0,1079)[0],0,1,"pan clamps to source left");
        check(v.pointer(-1,10)==null && v.pointer(1920,10)==null,"view boundary");
        v.update(1280,720,1280,720,1,0,0);
        int[] edge=v.pointer(1279.9f,719.9f);
        check(edge[0]==65535 && edge[1]==65535,"last pixel clamp");
        check(v.pointer(Float.NaN,1)==null,"non-finite touch");
        int[] drag=v.dragPointer(-200,900);
        check(drag[0]==0 && drag[1]==65535,"drag outside clamps rather than leaving button down");
        check(v.dragPointer(Float.POSITIVE_INFINITY,1)==null,"non-finite drag");
        v.update(0,0,1280,720,1,0,0);check(v.pointer(640,360)==null,"uninitialized frame");
        System.out.println(checks+" viewport regression checks passed");
    }
}
