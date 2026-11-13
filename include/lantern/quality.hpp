#pragma once
#include "core.hpp"
namespace lantern {
// Width presets are ceilings; aspect ratio, monitor refresh and decoder caps are negotiated separately.
struct QualitySettings { uint32_t width=1920,fps=60;bool automatic=false; };
struct QualityStatus {
    uint32_t width=1920,fps=60,bitrate=12'000'000,selected_width=1920;
    bool constrained=false,dropping=false;
    std::string codec="h264",path="tcp";
};
class AdaptiveQuality {
    QualitySettings settings_;
    QualityStatus status_;
    uint32_t ceiling_=3840;
    uint64_t bad_since_=0,good_since_=0;
    static uint32_t lower(uint32_t w) {return w>2560?2560:w>1920?1920:1280;}
    static uint32_t higher(uint32_t w) {return w<1920?1920:w<2560?2560:3840;}
  public:
    static void validate(QualitySettings s) {
        if((s.width!=1280&&s.width!=1920&&s.width!=2560&&s.width!=3840)||
           (s.fps!=30&&s.fps!=60&&s.fps!=120))throw Error(Code::invalid,"Quality preset");
    }
    static uint32_t target(uint32_t width,uint32_t fps) {
        // Text-oriented initial budgets: 720/1080/1440/2160 at 60 Hz = 5/12/22/45 Mbps.
        uint64_t base=width<=1280?5'000'000:width<=1920?12'000'000:width<=2560?22'000'000:45'000'000;
        return uint32_t(std::min<uint64_t>(160'000'000,base*fps/60));
    }
    void configure(QualitySettings settings,uint32_t ceiling,uint64_t initial_bps=0) {
        validate(settings);settings_=settings;ceiling_=ceiling;bad_since_=good_since_=0;
        auto w=std::min(settings.width,ceiling_);
        if(settings.automatic){w=1280;for(auto candidate:{1920u,2560u,3840u})
            if(candidate<=ceiling_&&initial_bps>uint64_t(target(candidate,settings.fps))*13/10)w=candidate;}
        status_.selected_width=w;status_.width=w;status_.fps=settings.fps;
        status_.bitrate=target(w,settings.fps);status_.constrained=status_.dropping=false;
    }
    const QualityStatus& state() const {return status_;}
    void sample(uint64_t now_ms,bool shortage,bool headroom) {
        auto target_rate=target(status_.selected_width,settings_.fps);
        if(shortage){
            good_since_=0;if(!bad_since_)bad_since_=now_ms;
            status_.constrained=true;
            status_.bitrate=std::max(250'000u,status_.bitrate*85/100);
            status_.dropping=status_.bitrate==250'000;
            if(settings_.automatic&&now_ms-bad_since_>=5000&&status_.selected_width>1280){
                status_.selected_width=lower(status_.selected_width);status_.width=status_.selected_width;
                status_.bitrate=std::min(status_.bitrate,target(status_.width,settings_.fps));bad_since_=now_ms;
            }
        } else {
            bad_since_=0;
            if(headroom){
                if(!good_since_)good_since_=now_ms;
                status_.bitrate=std::min(target_rate,std::max(status_.bitrate+250'000,status_.bitrate*105/100));
                if(settings_.automatic&&now_ms-good_since_>=15000&&higher(status_.selected_width)<=ceiling_){
                    status_.selected_width=higher(status_.selected_width);status_.width=status_.selected_width;
                    status_.bitrate=target(status_.width,settings_.fps);good_since_=now_ms;
                }
                status_.constrained=false;status_.dropping=false;
            }else good_since_=0;
        }
    }
};
} // namespace lantern
