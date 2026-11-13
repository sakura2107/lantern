#pragma once
#include "win.hpp"
namespace lantern {
struct MediaTrack {uint32_t index,kind;std::string codec,language,title;}; // kind1 audio,2 subtitle
struct MediaTracks {
    FileInfo source;uint64_t duration_ms=0,bitrate=0;uint32_t width=0,height=0;bool hdr=false;
    std::vector<MediaTrack> tracks;
};
struct MediaAdaptOptions {uint32_t width=1920,height=1080;int32_t audio=-1,subtitle=-1;}; // audio -1 default,-2 none; subtitle -1 none
MediaTracks media_tracks(MediaSource,std::function<bool()> cancelled);
Bytes media_segment(MediaSource,const std::string& expected_version,MediaAdaptOptions,uint32_t segment,
                    std::function<bool()> cancelled); // Independent 2s MPEG-TS segment, fixed codec/timestamps.
} // namespace lantern
