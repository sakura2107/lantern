#include "lantern/media_transcode.hpp"
#include "lantern/media_frame_guard.hpp"
#include <cmath>
#include <cctype>
#include <cstring>
#include <deque>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/common.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <ass/ass.h>
}
namespace lantern {
namespace {
void ff(int value,const char* where){if(value<0){if(value==AVERROR_EXIT)throw Error(Code::cancelled,"Media operation cancelled");char error[AV_ERROR_MAX_STRING_SIZE]{};av_strerror(value,error,sizeof(error));throw Error(Code::unsupported,std::string(where)+": "+error);}}
void check_cancel(const std::function<bool()>& stop){if(stop())throw Error(Code::cancelled,"Media operation cancelled");}
struct Frame {AVFrame* p=av_frame_alloc();~Frame(){av_frame_free(&p);}Frame(){if(!p)throw std::bad_alloc();}};
struct Packet {AVPacket* p=av_packet_alloc();~Packet(){av_packet_free(&p);}Packet(){if(!p)throw std::bad_alloc();}};
struct Codec {AVCodecContext* p=nullptr;~Codec(){avcodec_free_context(&p);}
    void create(const AVCodec* codec){if(!codec)throw Error(Code::unsupported,"Required media codec unavailable");p=avcodec_alloc_context3(codec);if(!p)throw std::bad_alloc();}};
struct Buffer {AVBufferRef* p=nullptr;~Buffer(){av_buffer_unref(&p);}};
struct Graph {AVFilterGraph* p=avfilter_graph_alloc();~Graph(){avfilter_graph_free(&p);}Graph(){if(!p)throw std::bad_alloc();}};
struct Resampler {SwrContext* p=nullptr;~Resampler(){swr_free(&p);}};
struct Fifo {AVAudioFifo* p=nullptr;~Fifo(){if(p)av_audio_fifo_free(p);}};
AudioInputFormat audio_format(int rate, int format, const AVChannelLayout& layout) {
    if (layout.nb_channels < 1 || layout.nb_channels > 16 || !av_channel_layout_check(&layout) ||
        format < 0 || format >= AV_SAMPLE_FMT_NB)
        throw Error(Code::unsupported, "Invalid resampler input format");
    AudioInputFormat result{rate, format, layout.nb_channels, int(layout.order)};
    if (layout.order == AV_CHANNEL_ORDER_NATIVE || layout.order == AV_CHANNEL_ORDER_AMBISONIC)
        result.channel_mask = layout.u.mask;
    if (layout.order == AV_CHANNEL_ORDER_CUSTOM)
        for (int i = 0; i < layout.nb_channels; ++i)
            result.custom_channels[i] = layout.u.map[i].id;
    result.planar = av_sample_fmt_is_planar(AVSampleFormat(format)) != 0;
    result.required_planes = result.planar ? result.channels : 1;
    result.bytes_per_sample = av_get_bytes_per_sample(AVSampleFormat(format));
    return result;
}
struct Input {
    MediaSource source;std::function<bool()> stop;AVFormatContext* format=nullptr;AVIOContext* io=nullptr;
    static int interrupted(void* self){try{return static_cast<Input*>(self)->stop()?1:0;}catch(...){return 1;}}
    static int read(void* self,uint8_t* buffer,int size){auto& in=*static_cast<Input*>(self);if(interrupted(self))return AVERROR_EXIT;
        DWORD got=0;if(!ReadFile(in.source.file,buffer,DWORD(size),&got,nullptr))return AVERROR(EIO);return got?int(got):AVERROR_EOF;}
    static int64_t seek(void* self,int64_t offset,int whence){auto& in=*static_cast<Input*>(self);
        if(whence==AVSEEK_SIZE)return int64_t(in.source.info.size);whence&=~AVSEEK_FORCE;
        if(whence!=SEEK_SET&&whence!=SEEK_CUR&&whence!=SEEK_END)return AVERROR(EINVAL);
        LARGE_INTEGER distance{},result{};distance.QuadPart=offset;
        return SetFilePointerEx(in.source.file,distance,&result,whence==SEEK_SET?FILE_BEGIN:whence==SEEK_CUR?FILE_CURRENT:FILE_END)?result.QuadPart:AVERROR(EIO);}
    void cleanup(){if(format)avformat_close_input(&format);if(io){av_freep(&io->buffer);avio_context_free(&io);}}
    Input(MediaSource file,std::function<bool()> cancelled):source(std::move(file)),stop(std::move(cancelled)){
        try{av_log_set_level(AV_LOG_ERROR);auto* bytes=static_cast<uint8_t*>(av_malloc(65536));if(!bytes)throw std::bad_alloc();
            io=avio_alloc_context(bytes,65536,0,this,read,nullptr,seek);if(!io){av_free(bytes);throw std::bad_alloc();}
            format=avformat_alloc_context();if(!format)throw std::bad_alloc();format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;
            format->interrupt_callback={interrupted,this};format->probesize=8*1024*1024;format->max_analyze_duration=5*AV_TIME_BASE;format->max_streams=64;
            ff(avformat_open_input(&format,nullptr,nullptr,nullptr),"Media container");ff(avformat_find_stream_info(format,nullptr),"Media tracks");
        }catch(...){cleanup();throw;}
    }
    ~Input(){cleanup();}
};
std::string metadata(AVDictionary* dictionary,const char* name){auto* entry=av_dict_get(dictionary,name,nullptr,0);return entry?std::string(entry->value).substr(0,1024):"";}
MediaTracks describe(Input& input){
    MediaTracks out;out.source=input.source.info;auto* format=input.format;
    out.duration_ms=format->duration>0?uint64_t(format->duration/1000):0;
    out.bitrate=format->bit_rate>0?uint64_t(format->bit_rate):out.duration_ms?out.source.size*8000/out.duration_ms:0;
    for(unsigned i=0;i<format->nb_streams;i++){auto* stream=format->streams[i];auto* codec=stream->codecpar;
        if(codec->codec_type==AVMEDIA_TYPE_VIDEO&&!out.width){out.width=uint32_t(codec->width);out.height=uint32_t(codec->height);
            out.hdr=codec->color_trc==AVCOL_TRC_SMPTE2084||codec->color_trc==AVCOL_TRC_ARIB_STD_B67;}
        if(codec->codec_type==AVMEDIA_TYPE_AUDIO||codec->codec_type==AVMEDIA_TYPE_SUBTITLE)
            out.tracks.push_back({i,codec->codec_type==AVMEDIA_TYPE_AUDIO?1u:2u,avcodec_get_name(codec->codec_id),metadata(stream->metadata,"language"),metadata(stream->metadata,"title")});
    }return out;
}
struct Mux {
    AVFormatContext* format=nullptr;AVIOContext* io=nullptr;Bytes bytes;
    static int write(void* self,const uint8_t* data,int size){auto& out=*static_cast<Mux*>(self);
        if(size<0||out.bytes.size()+size>max_frame-512)return AVERROR(ENOSPC);
        out.bytes.insert(out.bytes.end(),data,data+size);return size;}
    Mux(){
        try{ff(avformat_alloc_output_context2(&format,nullptr,"mpegts",nullptr),"MPEG-TS output");auto* buffer=static_cast<uint8_t*>(av_malloc(65536));if(!buffer)throw std::bad_alloc();
            io=avio_alloc_context(buffer,65536,1,this,nullptr,write,nullptr);if(!io){av_free(buffer);throw std::bad_alloc();}format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;
            format->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
        }catch(...){cleanup();throw;}
    }
    void cleanup(){if(format)avformat_free_context(format);if(io){av_freep(&io->buffer);avio_context_free(&io);}}
    ~Mux(){cleanup();}
    AVStream* add(AVCodecContext* context){auto* stream=avformat_new_stream(format,nullptr);if(!stream)throw std::bad_alloc();ff(avcodec_parameters_from_context(stream->codecpar,context),"Output stream parameters");stream->time_base=context->time_base;return stream;}
    // Timestamps are already global; the muxer must not rebase them per segment.
    void header(){AVDictionary* options=nullptr;av_dict_set(&options,"mpegts_copyts","1",0);
        auto result=avformat_write_header(format,&options);av_dict_free(&options);ff(result,"Segment header");}
    // keep_from/keep_until (encoder time base, before the global offset) drop audio pre/post-roll packets.
    void packets(AVCodecContext* context,AVStream* stream,int64_t keep_from=INT64_MIN,int64_t keep_until=INT64_MAX){Packet packet;
        while(true){int result=avcodec_receive_packet(context,packet.p);if(result==AVERROR(EAGAIN)||result==AVERROR_EOF)break;ff(result,"Encoded media packet");
            if(packet.p->pts!=AV_NOPTS_VALUE&&(packet.p->pts<keep_from||packet.p->pts>=keep_until)){av_packet_unref(packet.p);continue;}
            // A one-picture tail still needs a duration so the segment's video length is known.
            if(context->codec_type==AVMEDIA_TYPE_VIDEO&&packet.p->duration<=0){
                auto rate=context->framerate.num>0&&context->framerate.den>0?context->framerate:AVRational{30,1};
                packet.p->duration=std::max<int64_t>(1,av_rescale_q(1,av_inv_q(rate),context->time_base));
            }
            // Keep one global timeline across independently encoded segments. A fixed one-second
            // origin keeps every timestamp positive without per-segment shifts.
            auto offset=av_rescale_q(AV_TIME_BASE,AV_TIME_BASE_Q,context->time_base);
            if(packet.p->pts!=AV_NOPTS_VALUE)packet.p->pts+=offset;if(packet.p->dts!=AV_NOPTS_VALUE)packet.p->dts+=offset;
            av_packet_rescale_ts(packet.p,context->time_base,stream->time_base);packet.p->stream_index=stream->index;ff(av_interleaved_write_frame(format,packet.p),"Segment packet");av_packet_unref(packet.p);}
    }
};
// Cache subtitle packets, never an absolute path or a shell filter expression. Entire cache <=16MiB.
struct SubtitlePacket {Bytes bytes;int64_t start,duration;};
struct SubtitleData {Bytes header;std::vector<SubtitlePacket> packets;std::vector<std::pair<std::string,Bytes>> fonts;size_t bytes=0;};
std::mutex subtitle_mutex;std::map<std::pair<std::string,int>,std::shared_ptr<SubtitleData>> subtitle_cache;
std::shared_ptr<SubtitleData> subtitles(Input& input,int index){
    if(index<0)return {};
    if(unsigned(index)>=input.format->nb_streams||input.format->streams[index]->codecpar->codec_id!=AV_CODEC_ID_ASS)
        throw Error(Code::unsupported,"Adapted subtitles currently require an ASS track; choose original playback for other formats");
    auto key=std::make_pair(input.source.info.version,index);std::lock_guard lock(subtitle_mutex);
    if(auto it=subtitle_cache.find(key);it!=subtitle_cache.end())return it->second;
    auto data=std::make_shared<SubtitleData>();auto* format=input.format;auto* stream=format->streams[index];
    if(stream->codecpar->extradata_size<=0||stream->codecpar->extradata_size>1024*1024)throw Error(Code::invalid,"ASS header bound");
    data->header.assign(stream->codecpar->extradata,stream->codecpar->extradata+stream->codecpar->extradata_size);data->bytes=data->header.size();
    for(unsigned i=0;i<format->nb_streams;i++){
        auto* current=format->streams[i];current->discard=i==unsigned(index)?AVDISCARD_DEFAULT:AVDISCARD_ALL;
        auto attachment_name=metadata(current->metadata,"filename");auto suffix=attachment_name.substr(attachment_name.find_last_of('.')==std::string::npos?attachment_name.size():attachment_name.find_last_of('.'));
        std::transform(suffix.begin(),suffix.end(),suffix.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(current->codecpar->codec_type==AVMEDIA_TYPE_ATTACHMENT&&current->codecpar->extradata_size>0&&
           (suffix==".ttf"||suffix==".otf"||suffix==".ttc"||suffix==".woff")){
            auto name=metadata(current->metadata,"filename");auto size=size_t(current->codecpar->extradata_size);
            if(size>4*1024*1024||data->bytes+size>8*1024*1024)throw Error(Code::busy,"Subtitle font cache bound");
            data->fonts.emplace_back(name,Bytes(current->codecpar->extradata,current->codecpar->extradata+size));data->bytes+=size;
        }
    }
    ff(av_seek_frame(format,-1,0,AVSEEK_FLAG_BACKWARD),"Subtitle timeline seek");Packet packet;
    while(av_read_frame(format,packet.p)>=0){check_cancel(input.stop);
        if(packet.p->stream_index==index){if(packet.p->size<0||data->bytes+packet.p->size>8*1024*1024||data->packets.size()>=100000)throw Error(Code::busy,"ASS timeline cache bound");
            data->packets.push_back({Bytes(packet.p->data,packet.p->data+packet.p->size),av_rescale_q(packet.p->pts,stream->time_base,{1,1000}),av_rescale_q(packet.p->duration,stream->time_base,{1,1000})});data->bytes+=packet.p->size;}
        av_packet_unref(packet.p);
    }
    for(unsigned i=0;i<format->nb_streams;i++)format->streams[i]->discard=AVDISCARD_DEFAULT;
    size_t total=data->bytes;for(auto& [_,entry]:subtitle_cache)total+=entry->bytes;
    while(!subtitle_cache.empty()&&(total>16*1024*1024||subtitle_cache.size()>=4)){auto it=subtitle_cache.begin();total-=it->second->bytes;subtitle_cache.erase(it);}
    subtitle_cache.emplace(key,data);return data;
}
struct Ass {
    ASS_Library* library=nullptr;ASS_Renderer* renderer=nullptr;ASS_Track* track=nullptr;
    Ass(const std::shared_ptr<SubtitleData>& data,int width,int height){if(!data)return;
        try{library=ass_library_init();if(!library)throw std::bad_alloc();ass_set_message_cb(library,[](int,const char*,va_list,void*){},nullptr);
            for(auto& [name,bytes]:data->fonts)ass_add_font(library,name.c_str(),reinterpret_cast<char*>(const_cast<uint8_t*>(bytes.data())),int(bytes.size()));
            renderer=ass_renderer_init(library);if(!renderer)throw std::bad_alloc();ass_set_frame_size(renderer,width,height);ass_set_fonts(renderer,nullptr,"sans-serif",ASS_FONTPROVIDER_DIRECTWRITE,nullptr,1);
            track=ass_new_track(library);if(!track)throw std::bad_alloc();ass_process_codec_private(track,reinterpret_cast<char*>(const_cast<uint8_t*>(data->header.data())),int(data->header.size()));
            for(auto& packet:data->packets)ass_process_chunk(track,reinterpret_cast<char*>(const_cast<uint8_t*>(packet.bytes.data())),int(packet.bytes.size()),packet.start,packet.duration);
        }catch(...){cleanup();throw;}
    }
    void cleanup(){if(track)ass_free_track(track);if(renderer)ass_renderer_done(renderer);if(library)ass_library_done(library);}
    ~Ass(){cleanup();}
    void draw(AVFrame* frame,int64_t ms){if(!renderer)return;int changed=0;
        for(auto* image=ass_render_frame(renderer,track,ms,&changed);image;image=image->next){
            int red=int(image->color>>24),green=int((image->color>>16)&255),blue=int((image->color>>8)&255);
            int opacity=255-int(image->color&255);
            int yy=std::clamp(16+((47*red+157*green+16*blue+128)>>8),0,255);
            int uu=std::clamp(128+((-26*red-87*green+112*blue+128)>>8),0,255);
            int vv=std::clamp(128+((112*red-102*green-10*blue+128)>>8),0,255);
            for(int y=0;y<image->h;y++)for(int x=0;x<image->w;x++){
                int px=image->dst_x+x,py=image->dst_y+y;if(px<0||py<0||px>=frame->width||py>=frame->height)continue;
                int alpha=image->bitmap[y*image->stride+x]*opacity/255;auto* pixel=frame->data[0]+py*frame->linesize[0]+px;
                *pixel=uint8_t((*pixel*(255-alpha)+yy*alpha+127)/255);
                // Accumulate each luma sample's quarter contribution into its 4:2:0 chroma pair.
                alpha=(alpha+2)/4;auto* uv=frame->data[1]+(py/2)*frame->linesize[1]+(px&~1);
                uv[0]=uint8_t((uv[0]*(255-alpha)+uu*alpha+127)/255);uv[1]=uint8_t((uv[1]*(255-alpha)+vv*alpha+127)/255);
            }
        }
    }
};
} // namespace
MediaTracks media_tracks(MediaSource source,std::function<bool()> stop){Input input(std::move(source),std::move(stop));return describe(input);}
Bytes media_segment(MediaSource source,const std::string& expected,MediaAdaptOptions options,uint32_t segment,std::function<bool()> stop){
    check_cancel(stop);
    if(source.info.version!=expected)throw Error(Code::source_changed,"Media source changed; reopen playback");
    if(options.width<320||options.width>2560||options.height<180||options.height>2160||segment>=43200)throw Error(Code::invalid,"Media adaptation bounds");
    Input input(std::move(source),std::move(stop));auto tracks=describe(input);
    if(tracks.hdr)throw Error(Code::unsupported,"HDR tone mapping is unavailable in adapted mode; use original playback");
    // Two-second segments; a remainder under half a second joins the previous segment (last one <=2.5s),
    // so no segment is a sliver holding one picture. Clients compute the same count: max(1,(ms+1499)/2000).
    uint64_t segment_count=std::max<uint64_t>(1,(tracks.duration_ms+1499)/2000);
    if(!tracks.duration_ms||segment>=segment_count)throw Error(Code::invalid,"Media segment outside duration");
    auto* format=input.format;int video=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);ff(video,"Video track");
    auto* video_stream=format->streams[video];auto codec_id=video_stream->codecpar->codec_id;
    if(video_stream->codecpar->width<2||video_stream->codecpar->height<2||video_stream->codecpar->width>8192||video_stream->codecpar->height>4320||
        uint64_t(video_stream->codecpar->width)*video_stream->codecpar->height>33'554'432)
        throw Error(Code::unsupported,"Source video dimensions exceed the GPU adaptation bound");
    if(codec_id!=AV_CODEC_ID_H264&&codec_id!=AV_CODEC_ID_HEVC)throw Error(Code::unsupported,"Adapted GPU decode requires H.264 or HEVC; use original playback");
    int audio_index=options.audio;
    if(audio_index==-1)audio_index=av_find_best_stream(format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
    if(audio_index>=0&&(unsigned(audio_index)>=format->nb_streams||format->streams[audio_index]->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO))throw Error(Code::invalid,"Selected audio track");
    auto subtitle=subtitles(input,options.subtitle);
    uint32_t width=std::min(uint32_t(video_stream->codecpar->width),options.width)&~1u;
    uint32_t height=uint32_t(uint64_t(video_stream->codecpar->height)*width/video_stream->codecpar->width)&~1u;
    if(height>options.height){width=(uint32_t(uint64_t(width)*options.height/height)&~1u);height=uint32_t(uint64_t(video_stream->codecpar->height)*width/video_stream->codecpar->width)&~1u;}
    if(!width||!height)throw Error(Code::invalid,"Media video dimensions");
    Ass ass(subtitle,int(width),int(height));
    Buffer device;ff(av_hwdevice_ctx_create(&device.p,AV_HWDEVICE_TYPE_CUDA,nullptr,nullptr,0),"NVIDIA decode device");
    Codec decoder;decoder.create(avcodec_find_decoder_by_name(codec_id==AV_CODEC_ID_HEVC?"hevc_cuvid":"h264_cuvid"));
    ff(avcodec_parameters_to_context(decoder.p,video_stream->codecpar),"Video decode parameters");decoder.p->hw_device_ctx=av_buffer_ref(device.p);
    decoder.p->get_format=[](AVCodecContext*,const AVPixelFormat* formats){for(auto* p=formats;*p!=AV_PIX_FMT_NONE;p++)if(*p==AV_PIX_FMT_CUDA)return *p;return AV_PIX_FMT_NONE;};
    ff(avcodec_open2(decoder.p,decoder.p->codec,nullptr),"NVDEC open");
    Codec audio_decoder,audio_encoder;Resampler resampler;Fifo fifo;AVStream* audio_stream=nullptr;
    AudioInputFormat resampler_input{};
    if(audio_index>=0){auto* parameters=format->streams[audio_index]->codecpar;audio_decoder.create(avcodec_find_decoder(parameters->codec_id));
        if(parameters->sample_rate<8000||parameters->sample_rate>192000||parameters->ch_layout.nb_channels<1||parameters->ch_layout.nb_channels>16)
            throw Error(Code::unsupported,"Source audio channel/rate bound; use original playback");
        ff(avcodec_parameters_to_context(audio_decoder.p,parameters),"Audio parameters");ff(avcodec_open2(audio_decoder.p,audio_decoder.p->codec,nullptr),"Audio decoder");
        audio_encoder.create(avcodec_find_encoder(AV_CODEC_ID_AAC));audio_encoder.p->sample_rate=48000;audio_encoder.p->time_base={1,48000};audio_encoder.p->sample_fmt=AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&audio_encoder.p->ch_layout,2);audio_encoder.p->bit_rate=160000;audio_encoder.p->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;ff(avcodec_open2(audio_encoder.p,audio_encoder.p->codec,nullptr),"AAC stereo encoder");
        resampler_input = audio_format(audio_decoder.p->sample_rate, audio_decoder.p->sample_fmt, audio_decoder.p->ch_layout);
        ff(swr_alloc_set_opts2(&resampler.p,&audio_encoder.p->ch_layout,AV_SAMPLE_FMT_FLTP,48000,&audio_decoder.p->ch_layout,audio_decoder.p->sample_fmt,audio_decoder.p->sample_rate,0,nullptr),"Audio resampler");ff(swr_init(resampler.p),"Audio resampler start");
        fifo.p=av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,2,4096);if(!fifo.p)throw std::bad_alloc();
    }
    auto origin=format->start_time==AV_NOPTS_VALUE?0:format->start_time;
    bool last_segment=segment+1==segment_count;
    int64_t begin=origin+int64_t(segment)*2*AV_TIME_BASE,end=last_segment?origin+int64_t(tracks.duration_ms)*1000:begin+2*AV_TIME_BASE;
    // Audio uses one global grid of 1024-sample AAC frames (48 kHz from the media origin). A segment keeps
    // the frames that start inside its two seconds, so consecutive segments tile without repeats or gaps.
    // Two frames of real audio are encoded before and after and then dropped, so the overlap windows at
    // each boundary come from the same signal and the player's continuous decoder hears no click.
    const int64_t grid=1024;
    int64_t keep_from=(int64_t(segment)*96000+grid-1)/grid*grid;
    int64_t keep_until=last_segment?INT64_MAX:(int64_t(segment+1)*96000+grid-1)/grid*grid;
    int64_t feed_from=std::max<int64_t>(0,keep_from-2*grid);
    int64_t feed_from_us=origin+av_rescale(feed_from,AV_TIME_BASE,48000);
    int64_t feed_until_us=last_segment?INT64_MAX:origin+av_rescale(keep_until+2*grid,AV_TIME_BASE,48000);
    // Seek a little early so the audio pre-roll is still ahead of the demuxer position.
    ff(av_seek_frame(format,video,av_rescale_q(std::max(origin,std::min(begin,feed_from_us)-AV_TIME_BASE/10),AV_TIME_BASE_Q,video_stream->time_base),AVSEEK_FLAG_BACKWARD),"Media seek");
    avcodec_flush_buffers(decoder.p);if(audio_decoder.p)avcodec_flush_buffers(audio_decoder.p);
    Codec encoder;Graph graph;AVFilterContext* buffers=nullptr,*sink=nullptr;Mux mux;AVStream* output_video=nullptr;
    if(audio_encoder.p)audio_stream=mux.add(audio_encoder.p);
    bool header=false,video_done=false,audio_done=audio_index<0,audio_started=false;int64_t audio_pts=feed_from;
    Frame last_before;
    auto audio_flush=[&](bool last){
        while(fifo.p&&av_audio_fifo_size(fifo.p)>0&&(last||av_audio_fifo_size(fifo.p)>=audio_encoder.p->frame_size)){
            Frame frame;frame.p->format=AV_SAMPLE_FMT_FLTP;frame.p->sample_rate=48000;ff(av_channel_layout_copy(&frame.p->ch_layout,&audio_encoder.p->ch_layout),"AAC frame layout");
            frame.p->nb_samples=std::min(av_audio_fifo_size(fifo.p),audio_encoder.p->frame_size);ff(av_frame_get_buffer(frame.p,0),"AAC frame buffer");
            ff(av_audio_fifo_read(fifo.p,reinterpret_cast<void**>(frame.p->data),frame.p->nb_samples),"Audio FIFO read");frame.p->pts=audio_pts;audio_pts+=frame.p->nb_samples;
            ff(avcodec_send_frame(audio_encoder.p,frame.p),"AAC encode");if(header)mux.packets(audio_encoder.p,audio_stream,keep_from,keep_until);
        }
    };
    auto video_frame=[&](AVFrame* frame){
        int64_t stamp=av_rescale_q(frame->best_effort_timestamp,video_stream->time_base,AV_TIME_BASE_Q);
        if(stamp<begin){av_frame_unref(last_before.p);ff(av_frame_ref(last_before.p,frame),"Last video surface");return;}
        if(stamp>=end){video_done=true;return;}
        if(!buffers){
            std::string args="video_size="+std::to_string(frame->width)+"x"+std::to_string(frame->height)+":pix_fmt="+std::to_string(AV_PIX_FMT_CUDA)+":time_base="+std::to_string(video_stream->time_base.num)+"/"+std::to_string(video_stream->time_base.den)+":pixel_aspect=1/1";
            ff(avfilter_graph_create_filter(&buffers,avfilter_get_by_name("buffer"),"input",args.c_str(),nullptr,graph.p),"GPU filter input");
            auto* parameters=av_buffersrc_parameters_alloc();if(!parameters)throw std::bad_alloc();parameters->hw_frames_ctx=av_buffer_ref(frame->hw_frames_ctx);
            auto result=av_buffersrc_parameters_set(buffers,parameters);av_buffer_unref(&parameters->hw_frames_ctx);av_free(parameters);ff(result,"GPU filter surfaces");
            AVFilterContext* scale=nullptr;args="w="+std::to_string(width)+":h="+std::to_string(height)+":format=nv12";
            ff(avfilter_graph_create_filter(&scale,avfilter_get_by_name("scale_cuda"),"scale",args.c_str(),nullptr,graph.p),"CUDA resize");
            ff(avfilter_graph_create_filter(&sink,avfilter_get_by_name("buffersink"),"output",nullptr,nullptr,graph.p),"GPU filter output");
            ff(avfilter_link(buffers,0,scale,0),"GPU filter link");ff(avfilter_link(scale,0,sink,0),"GPU output link");ff(avfilter_graph_config(graph.p,nullptr),"GPU filter configure");
        }
        frame->pts=frame->best_effort_timestamp;ff(av_buffersrc_add_frame_flags(buffers,frame,AV_BUFFERSRC_FLAG_KEEP_REF),"GPU filter push");Frame resized;
        ff(av_buffersink_get_frame(sink,resized.p),"GPU filter frame");
        if(!encoder.p){
            // H.264 High in MPEG-TS: the only HLS video form Android's system player (and every
            // external player) accepts. SPS/PPS travel in-band with each segment's IDR.
            encoder.create(avcodec_find_encoder_by_name("h264_nvenc"));encoder.p->width=int(width);encoder.p->height=int(height);encoder.p->pix_fmt=AV_PIX_FMT_CUDA;
            encoder.p->hw_frames_ctx=av_buffer_ref(resized.p->hw_frames_ctx);encoder.p->time_base={1,90000};encoder.p->framerate=av_guess_frame_rate(format,video_stream,nullptr);
            double fps=encoder.p->framerate.num>0&&encoder.p->framerate.den>0?std::clamp(av_q2d(encoder.p->framerate),1.0,120.0):30.0;
            // About 0.1 bit per pixel, never far above the source; the 8 Mbps cap keeps a worst-case
            // 2.5-second final segment (rate*2.5 + one-second buffer) inside one 4 MiB protocol frame.
            uint64_t wanted=uint64_t(double(width)*height*fps*0.1);if(tracks.bitrate)wanted=std::min<uint64_t>(wanted,tracks.bitrate*6/5);
            encoder.p->bit_rate=int64_t(std::clamp<uint64_t>(wanted,1000000,8000000));encoder.p->rc_max_rate=encoder.p->bit_rate;encoder.p->rc_buffer_size=int(encoder.p->bit_rate);
            encoder.p->gop_size=std::max(1,int(std::ceil(fps*2)));encoder.p->max_b_frames=0;
            ff(av_opt_set(encoder.p->priv_data,"preset","p4",0),"Media NVENC preset");ff(av_opt_set(encoder.p->priv_data,"tune","hq",0),"Media NVENC tune");
            ff(av_opt_set(encoder.p->priv_data,"profile","high",0),"Media NVENC profile");ff(av_opt_set(encoder.p->priv_data,"aud","1",0),"Media NVENC delimiters");
            ff(avcodec_open2(encoder.p,encoder.p->codec,nullptr),"Media NVENC open");output_video=mux.add(encoder.p);mux.header();header=true;
        }
        if(subtitle){Frame cpu;ff(av_hwframe_transfer_data(cpu.p,resized.p,0),"Subtitle surface download");
            if(cpu.p->format!=AV_PIX_FMT_NV12)throw Error(Code::unsupported,"ASS compositor requires NV12");ass.draw(cpu.p,stamp/1000);
            Frame uploaded;ff(av_hwframe_get_buffer(resized.p->hw_frames_ctx,uploaded.p,0),"Subtitle GPU surface");ff(av_hwframe_transfer_data(uploaded.p,cpu.p,0),"Subtitle surface upload");
            av_frame_unref(resized.p);av_frame_move_ref(resized.p,uploaded.p);
        }
        resized.p->pts=av_rescale_q(stamp-origin,AV_TIME_BASE_Q,encoder.p->time_base);
        ff(avcodec_send_frame(encoder.p,resized.p),"Media NVENC encode");mux.packets(encoder.p,output_video);audio_flush(false);
    };
    auto audio_frame=[&](AVFrame* frame){
        auto* stream=format->streams[audio_index];int64_t stamp=av_rescale_q(frame->best_effort_timestamp,stream->time_base,AV_TIME_BASE_Q);
        auto actual = audio_format(frame->sample_rate, frame->format, frame->ch_layout);
        std::span<const uint8_t* const> planes;
        if (frame->extended_data && actual == resampler_input) {
            // FFmpeg's separate extended_data table has one entry per input plane.
            auto count = frame->extended_data == frame->data
                ? std::min(actual.required_planes, AV_NUM_DATA_POINTERS) : actual.required_planes;
            planes = {frame->extended_data, static_cast<size_t>(count)};
        }
        if (!valid_audio_frame(resampler_input, actual, planes, frame->nb_samples, 0))
            throw Error(Code::unsupported, "Audio frame incompatible with resampler input");
        if(stamp>=feed_until_us){audio_done=true;return;}
        auto duration = av_rescale(frame->nb_samples, AV_TIME_BASE, actual.sample_rate);
        if (av_sat_add64(stamp, duration) <= feed_from_us) return;
        auto skip = stamp < feed_from_us ? av_rescale(feed_from_us-stamp, actual.sample_rate, AV_TIME_BASE) : 0;
        if (!valid_audio_frame(resampler_input, actual, planes, frame->nb_samples, skip))
            throw Error(Code::unsupported, "Audio frame trim outside input bounds");
        int samples = frame->nb_samples - int(skip);
        if (feed_until_us != INT64_MAX)
            samples = int(std::min<int64_t>(samples, av_rescale(feed_until_us-std::max(stamp,feed_from_us),actual.sample_rate,AV_TIME_BASE)));
        if(samples<=0)return;
        if(!audio_started){audio_started=true;
            // Audio that starts after the pre-roll point (stream start, or a late first packet) is
            // placed at its real time by leading silence instead of being pulled earlier.
            int64_t silence=av_rescale(std::max(stamp,feed_from_us)-feed_from_us,48000,AV_TIME_BASE);
            if(silence>0){if(silence>48000*3)throw Error(Code::unsupported,"Audio starts far after the requested position");
                Frame quiet;quiet.p->format=AV_SAMPLE_FMT_FLTP;quiet.p->sample_rate=48000;av_channel_layout_default(&quiet.p->ch_layout,2);quiet.p->nb_samples=int(silence);
                ff(av_frame_get_buffer(quiet.p,0),"Audio silence buffer");ff(av_samples_set_silence(quiet.p->data,0,quiet.p->nb_samples,2,AV_SAMPLE_FMT_FLTP),"Audio silence");
                ff(av_audio_fifo_realloc(fifo.p,av_audio_fifo_size(fifo.p)+quiet.p->nb_samples),"Audio FIFO capacity");
                ff(av_audio_fifo_write(fifo.p,reinterpret_cast<void**>(quiet.p->data),quiet.p->nb_samples),"Audio silence write");}
        }
        int stride = actual.bytes_per_sample * (actual.planar ? 1 : actual.channels);
        if (frame->linesize[0] < frame->nb_samples * stride)
            throw Error(Code::unsupported, "Audio frame plane too short");
        std::array<const uint8_t*, 16> data{};
        for (int i = 0; i < actual.required_planes; ++i)
            data[i] = planes[i] + skip * stride;
        Frame converted;converted.p->format=AV_SAMPLE_FMT_FLTP;converted.p->sample_rate=48000;av_channel_layout_default(&converted.p->ch_layout,2);
        converted.p->nb_samples=int(av_rescale_rnd(swr_get_delay(resampler.p,frame->sample_rate)+samples,48000,frame->sample_rate,AV_ROUND_UP));
        ff(av_frame_get_buffer(converted.p,0),"Audio conversion buffer");int got=swr_convert(resampler.p,converted.p->data,converted.p->nb_samples,data.data(),samples);ff(got,"Stereo resample");
        if(av_audio_fifo_size(fifo.p)+got>48000*4)throw Error(Code::busy,"Bounded audio mux queue");
        ff(av_audio_fifo_realloc(fifo.p,av_audio_fifo_size(fifo.p)+got),"Audio FIFO capacity");ff(av_audio_fifo_write(fifo.p,reinterpret_cast<void**>(converted.p->data),got),"Audio FIFO write");if(header)audio_flush(false);
    };
    Packet packet;Frame decoded;
    while((!video_done||!audio_done)&&av_read_frame(format,packet.p)>=0){check_cancel(input.stop);
        AVCodecContext* context=packet.p->stream_index==video&&!video_done?decoder.p:packet.p->stream_index==audio_index&&!audio_done?audio_decoder.p:nullptr;
        if(context){ff(avcodec_send_packet(context,packet.p),"Media decode packet");while(true){int result=avcodec_receive_frame(context,decoded.p);if(result==AVERROR(EAGAIN)||result==AVERROR_EOF)break;ff(result,"Media decode frame");
                if(context==decoder.p)video_frame(decoded.p);else audio_frame(decoded.p);av_frame_unref(decoded.p);}}
        av_packet_unref(packet.p);
    }
    ff(avcodec_send_packet(decoder.p,nullptr),"NVDEC drain");while(avcodec_receive_frame(decoder.p,decoded.p)>=0){video_frame(decoded.p);av_frame_unref(decoded.p);}
    // Audio padding can outlast the final video sample (e.g. a 32 ms last HLS segment). Preserve the
    // complete timeline by holding its last picture instead of serving an unplayable empty segment.
    if(!header&&last_before.p->width>0){last_before.p->best_effort_timestamp=av_rescale_q(begin,AV_TIME_BASE_Q,video_stream->time_base);video_frame(last_before.p);}
    if(!header)throw Error(Code::unsupported,"No decodable video at requested position");
    if(audio_decoder.p){ff(avcodec_send_packet(audio_decoder.p,nullptr),"Audio decoder drain");while(avcodec_receive_frame(audio_decoder.p,decoded.p)>=0){audio_frame(decoded.p);av_frame_unref(decoded.p);}audio_flush(true);
        ff(avcodec_send_frame(audio_encoder.p,nullptr),"AAC drain");mux.packets(audio_encoder.p,audio_stream,keep_from,keep_until);}
    ff(avcodec_send_frame(encoder.p,nullptr),"NVENC drain");mux.packets(encoder.p,output_video);ff(av_write_trailer(mux.format),"Segment finish");avio_flush(mux.io);
    check_cancel(input.stop);return std::move(mux.bytes);
}
} // namespace lantern
