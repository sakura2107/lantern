package org.lantern.client;
import android.media.*;
import android.os.Build;
import java.util.*;
/** Queries advertised hardware capabilities; no assumption based on phone model. */
final class DecoderCapabilities {
    static final class Limit {
        final String mime,name;final int width,height,fps;
        Limit(String m,String n,int w,int h,int f){mime=m;name=n;width=w;height=h;fps=f;}
    }
    static List<Limit> query(){
        List<Limit> out=new ArrayList<>();
        for(MediaCodecInfo info:new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if(info.isEncoder()||(Build.VERSION.SDK_INT>=29&&!info.isHardwareAccelerated()))continue;
            for(String mime:info.getSupportedTypes()) {
                if(!mime.equals("video/avc")&&!mime.equals("video/hevc"))continue;
                try{
                    MediaCodecInfo.VideoCapabilities caps=info.getCapabilitiesForType(mime).getVideoCapabilities();
                    for(int width:new int[]{1280,1920,2560,3840})for(int fps:new int[]{30,60,120}) {
                        int height=(width*9/16)&~1;
                        if(caps.areSizeAndRateSupported(width,height,fps))out.add(new Limit(mime,info.getName(),width,height,fps));
                    }
                }catch(IllegalArgumentException ignored){}
            }
        }
        return Collections.unmodifiableList(out);
    }
    static Limit best(boolean preferHevc,int width,int height,int fps) {
        for(String mime:preferHevc?new String[]{"video/hevc","video/avc"}:new String[]{"video/avc"}){
            Limit best=null;
            for(Limit l:query())if(l.mime.equals(mime)&&l.width<=Math.max(1280,width)&&l.height<=Math.max(720,height)&&l.fps<=fps&&
                (best==null||l.fps>best.fps||l.fps==best.fps&&l.width>best.width))best=l;
            if(best!=null)return best;
        }
        return null;
    }
}
