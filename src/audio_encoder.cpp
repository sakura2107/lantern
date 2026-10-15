#include "lantern/win.hpp"
#ifdef LANTERN_WITH_OPUS
#include <opus.h>
#include <array>
#include <cstring>
#endif
namespace lantern {
#ifdef LANTERN_WITH_OPUS
class OpusAudioEncoder final : public AudioEncoder {
    struct Deleter { void operator()(OpusEncoder* value) const { opus_encoder_destroy(value); } };
    std::unique_ptr<OpusEncoder,Deleter> encoder;
    uint32_t delay=0;
public:
    OpusAudioEncoder(){int error=0;encoder.reset(opus_encoder_create(48000,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&error));
        if(error!=OPUS_OK||!encoder)throw Error(Code::unsupported,"Opus encoder unavailable");
        if(opus_encoder_ctl(encoder.get(),OPUS_SET_BITRATE(96000))!=OPUS_OK||opus_encoder_ctl(encoder.get(),OPUS_SET_VBR(0))!=OPUS_OK||opus_encoder_ctl(encoder.get(),OPUS_SET_COMPLEXITY(5))!=OPUS_OK)throw Error(Code::unsupported,"Opus configuration failed");
        int frames=0;if(opus_encoder_ctl(encoder.get(),OPUS_GET_LOOKAHEAD(&frames))!=OPUS_OK)throw Error(Code::unsupported,"Opus delay unavailable");delay=uint32_t(frames);
    }
    uint32_t lookahead_frames()const override{return delay;}
    Bytes encode(std::span<const uint8_t> pcm) override{
        if(pcm.size()!=1920)throw Error(Code::invalid,"Opus requires 480 stereo PCM16 frames");
        std::array<opus_int16,960> samples;std::memcpy(samples.data(),pcm.data(),pcm.size());Bytes output(1275);
        int count=opus_encode(encoder.get(),samples.data(),480,output.data(),int(output.size()));if(count<0)throw Error(Code::io,"Opus encode failed");output.resize(size_t(count));return output;
    }
};
#endif
std::unique_ptr<AudioEncoder> open_opus_encoder(uint32_t sample_rate){
#ifdef LANTERN_WITH_OPUS
    if(sample_rate==48000)return std::make_unique<OpusAudioEncoder>();
#else
    (void)sample_rate;
#endif
    return {};
}
}
