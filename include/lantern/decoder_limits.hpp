#pragma once
#include "core.hpp"
namespace lantern {
struct DecoderMode {uint32_t width,height,fps;};
// Preserve size/rate pairs: 4K60 + 1440p120 must never advertise 4K120.
inline DecoderMode decoder_limit(const std::vector<DecoderMode>& modes,uint32_t requested_fps,
                                 uint32_t display_width,uint32_t display_height) {
    DecoderMode result{};
    for(const auto& mode:modes) {
        if(mode.width<640||mode.width>3840||mode.height<360||mode.height>2160||!mode.fps||mode.fps>120)
            throw Error(Code::invalid,"Decoder mode bound");
        auto fps=std::min(requested_fps,mode.fps);
        auto width=std::min(mode.width,uint32_t(uint64_t(mode.height)*display_width/display_height));
        if(fps>result.fps||(fps==result.fps&&width>result.width))result={width,mode.height,fps};
    }
    if(!result.fps)throw Error(Code::invalid,"Empty decoder modes");
    return result;
}
}
