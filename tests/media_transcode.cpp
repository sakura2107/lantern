#include "lantern/media_transcode.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
using namespace lantern;
static int checks=0;static void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
static void ok(int v,const char* s){if(v<0){char message[128];av_strerror(v,message,sizeof(message));throw std::runtime_error(std::string(s)+": "+message);}}
static void packets(AVFormatContext* output,AVCodecContext* encoder,AVStream* stream){AVPacket* packet=av_packet_alloc();
    while(avcodec_receive_packet(encoder,packet)>=0){av_packet_rescale_ts(packet,encoder->time_base,stream->time_base);packet->stream_index=stream->index;ok(av_interleaved_write_frame(output,packet),"fixture mux");av_packet_unref(packet);}av_packet_free(&packet);}
static void fixture(const fs::path& path,int seconds=4){
    AVFormatContext* output=nullptr;AVCodecContext* video=nullptr,*audio=nullptr;AVFrame* image=nullptr,*sound=nullptr;
    auto cleanup=[&]{av_frame_free(&image);av_frame_free(&sound);avcodec_free_context(&video);avcodec_free_context(&audio);if(output){if(output->pb)avio_closep(&output->pb);avformat_free_context(output);}};
    try{
        ok(avformat_alloc_output_context2(&output,nullptr,"mp4",utf8(path.wstring()).c_str()),"fixture container");
        auto* codec=avcodec_find_encoder_by_name("hevc_nvenc");check(codec!=nullptr,"static NVENC encoder linked");video=avcodec_alloc_context3(codec);
        video->width=3840;video->height=2160;video->pix_fmt=AV_PIX_FMT_P010LE;video->time_base={1,30};video->framerate={30,1};video->bit_rate=8'000'000;video->gop_size=60;video->max_b_frames=0;video->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
        video->color_trc=AVCOL_TRC_BT709;video->color_primaries=AVCOL_PRI_BT709;video->colorspace=AVCOL_SPC_BT709;
        ok(av_opt_set(video->priv_data,"preset","p4",0),"fixture preset");ok(av_opt_set(video->priv_data,"profile","main10",0),"fixture main10");ok(avcodec_open2(video,codec,nullptr),"fixture HEVC Main10 encoder");
        auto* vs=avformat_new_stream(output,nullptr);vs->time_base=video->time_base;ok(avcodec_parameters_from_context(vs->codecpar,video),"fixture video stream");
        codec=avcodec_find_encoder(AV_CODEC_ID_AAC);audio=avcodec_alloc_context3(codec);audio->sample_rate=48000;audio->sample_fmt=AV_SAMPLE_FMT_FLTP;audio->time_base={1,48000};audio->bit_rate=128000;av_channel_layout_default(&audio->ch_layout,2);audio->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
        ok(avcodec_open2(audio,codec,nullptr),"fixture AAC");auto* as=avformat_new_stream(output,nullptr);as->time_base=audio->time_base;ok(avcodec_parameters_from_context(as->codecpar,audio),"fixture audio stream");
        ok(avio_open(&output->pb,utf8(path.wstring()).c_str(),AVIO_FLAG_WRITE),"fixture output");ok(avformat_write_header(output,nullptr),"fixture header");
        image=av_frame_alloc();image->format=video->pix_fmt;image->width=video->width;image->height=video->height;ok(av_frame_get_buffer(image,0),"fixture pixels");
        sound=av_frame_alloc();sound->format=audio->sample_fmt;sound->sample_rate=48000;sound->nb_samples=audio->frame_size;av_channel_layout_default(&sound->ch_layout,2);ok(av_frame_get_buffer(sound,0),"fixture sound");
        int64_t samples=0;
        for(int n=0;n<seconds*30;n++){
            ok(av_frame_make_writable(image),"fixture image writable");
            for(int y=0;y<2160;y++){auto* row=reinterpret_cast<uint16_t*>(image->data[0]+y*image->linesize[0]);for(int x=0;x<3840;x++)row[x]=uint16_t((180+(x/16+y/16+n)%600)<<6);}
            for(int y=0;y<1080;y++){auto* row=reinterpret_cast<uint16_t*>(image->data[1]+y*image->linesize[1]);std::fill_n(row,3840,uint16_t(512<<6));}
            image->pts=n;ok(avcodec_send_frame(video,image),"fixture encode");packets(output,video,vs);
            while(samples<(n+1)*48000/30){ok(av_frame_make_writable(sound),"fixture sound writable");
                for(int i=0;i<sound->nb_samples;i++)for(int ch=0;ch<2;ch++)reinterpret_cast<float*>(sound->data[ch])[i]=float(.04*std::sin((samples+i)*6.28318530718*997/48000));
                sound->pts=samples;samples+=sound->nb_samples;ok(avcodec_send_frame(audio,sound),"fixture AAC packet");packets(output,audio,as);}
        }
        ok(avcodec_send_frame(video,nullptr),"fixture video flush");packets(output,video,vs);ok(avcodec_send_frame(audio,nullptr),"fixture audio flush");packets(output,audio,as);ok(av_write_trailer(output),"fixture finish");cleanup();
    }catch(...){cleanup();throw;}
}
static Bytes element(uint32_t id,const Bytes& value){Bytes out;int count=id>0xffffff?4:id>0xffff?3:id>0xff?2:1;for(int i=count-1;i>=0;i--)out.push_back(uint8_t(id>>(8*i)));
    uint64_t size=value.size();int n=1;while(n<8&&size>=(uint64_t(1)<<(7*n))-1)n++;uint64_t encoded=size|(uint64_t(1)<<(7*n));for(int i=n-1;i>=0;i--)out.push_back(uint8_t(encoded>>(8*i)));out.insert(out.end(),value.begin(),value.end());return out;}
static Bytes number(uint32_t id,uint64_t n){Bytes bytes;do{bytes.insert(bytes.begin(),uint8_t(n));n>>=8;}while(n);return element(id,bytes);}
static Bytes text(uint32_t id,const std::string& s){return element(id,Bytes(s.begin(),s.end()));}
static void append(Bytes& a,const Bytes& b){a.insert(a.end(),b.begin(),b.end());}
static Bytes floating(uint32_t id,double x){uint64_t bits;memcpy(&bits,&x,8);Bytes b;for(int i=7;i>=0;i--)b.push_back(uint8_t(bits>>(8*i)));return element(id,b);}
static void mkv(const fs::path& mp4,const fs::path& destination){
    AVFormatContext* input=nullptr;ok(avformat_open_input(&input,utf8(mp4.wstring()).c_str(),nullptr,nullptr),"fixture MP4 read");
    auto cleanup=[&]{avformat_close_input(&input);};
    try{ok(avformat_find_stream_info(input,nullptr),"fixture stream info");Bytes ebml;
        for(auto pair:{std::pair<uint32_t,int>{0x4286,1},{0x42f7,1},{0x42f2,4},{0x42f3,8},{0x4287,4},{0x4285,2}})append(ebml,number(pair.first,pair.second));append(ebml,text(0x4282,"matroska"));
        double duration=input->duration*1000.0/AV_TIME_BASE;
        Bytes result=element(0x1a45dfa3,ebml),body,info;append(info,number(0x2ad7b1,1000000));append(info,floating(0x4489,duration));append(info,text(0x4d80,"Lantern generated fixture"));append(body,element(0x1549a966,info));
        Bytes tracks;
        for(unsigned i=0;i<input->nb_streams;i++){
            auto* p=input->streams[i]->codecpar;Bytes entry;append(entry,number(0xd7,i+1));append(entry,number(0x73c5,i+1));append(entry,number(0x83,p->codec_type==AVMEDIA_TYPE_VIDEO?1:2));append(entry,number(0x9c,0));
            append(entry,text(0x86,p->codec_type==AVMEDIA_TYPE_VIDEO?"V_MPEGH/ISO/HEVC":"A_AAC"));append(entry,element(0x63a2,Bytes(p->extradata,p->extradata+p->extradata_size)));
            if(p->codec_type==AVMEDIA_TYPE_VIDEO){Bytes video;append(video,number(0xb0,p->width));append(video,number(0xba,p->height));append(entry,element(0xe0,video));}
            else{Bytes audio;append(audio,floating(0xb5,48000));append(audio,number(0x9f,2));append(entry,element(0xe1,audio));}
            append(tracks,element(0xae,entry));
        }
        const std::string header="[Script Info]\nScriptType: v4.00+\nPlayResX: 3840\nPlayResY: 2160\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,140,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,3,0,2,30,30,70,1\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        Bytes sub;append(sub,number(0xd7,3));append(sub,number(0x73c5,3));append(sub,number(0x83,0x11));append(sub,text(0x86,"S_TEXT/ASS"));append(sub,text(0x63a2,header));append(sub,text(0x536e,"ASS generated test"));append(tracks,element(0xae,sub));append(body,element(0x1654ae6b,tracks));
        Bytes cluster;append(cluster,number(0xe7,0));
        std::string dialogue="0,0,Default,,0,0,0,,Lantern ASS Burn Test";Bytes block{0x83,0,0,0};block.insert(block.end(),dialogue.begin(),dialogue.end());Bytes group=element(0xa1,block);append(group,number(0x9b,uint64_t(duration)));append(cluster,element(0xa0,group));
        AVPacket* packet=av_packet_alloc();while(av_read_frame(input,packet)>=0){int64_t ms=av_rescale_q(packet->pts,input->streams[packet->stream_index]->time_base,{1,1000});
            if(ms>=0&&ms<32767){Bytes raw{uint8_t(0x80|uint8_t(packet->stream_index+1)),uint8_t(ms>>8),uint8_t(ms),uint8_t(packet->flags&AV_PKT_FLAG_KEY?0x80:0)};raw.insert(raw.end(),packet->data,packet->data+packet->size);append(cluster,element(0xa3,raw));}av_packet_unref(packet);}
        av_packet_free(&packet);append(body,element(0x1f43b675,cluster));append(result,element(0x18538067,body));write_file(destination,result);cleanup();
    }catch(...){cleanup();throw;}
}
static Bytes decoded_luma(const fs::path& path){AVFormatContext* format=nullptr;AVCodecContext* decoder=nullptr;AVPacket* packet=av_packet_alloc();AVFrame* frame=av_frame_alloc();Bytes pixels;
    auto cleanup=[&]{av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&decoder);avformat_close_input(&format);};
    try{ok(avformat_open_input(&format,utf8(path.wstring()).c_str(),nullptr,nullptr),"segment demux");ok(avformat_find_stream_info(format,nullptr),"segment tracks");auto index=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);ok(index,"segment video");
        auto* codec=avcodec_find_decoder(AV_CODEC_ID_HEVC);decoder=avcodec_alloc_context3(codec);ok(avcodec_parameters_to_context(decoder,format->streams[index]->codecpar),"segment decode params");ok(avcodec_open2(decoder,codec,nullptr),"segment decode");
        while(pixels.empty()&&av_read_frame(format,packet)>=0){if(packet->stream_index==index){check(packet->duration>0,"fMP4 video sample has nonzero duration including one-frame tail");ok(avcodec_send_packet(decoder,packet),"segment decode packet");if(avcodec_receive_frame(decoder,frame)>=0){check(frame->width==1280&&frame->height==720,"GPU adaptation resizes to client target");check(frame->format==AV_PIX_FMT_YUV420P,"10-bit source adapted to 8-bit compatible HEVC");for(int y=0;y<frame->height;y++)pixels.insert(pixels.end(),frame->data[0]+y*frame->linesize[0],frame->data[0]+y*frame->linesize[0]+frame->width);}}
            av_packet_unref(packet);}
        check(!pixels.empty(),"adapted segment independently decodes");cleanup();return pixels;
    }catch(...){cleanup();throw;}
}
static std::vector<uint64_t> decode_times(const Bytes& bytes){std::vector<uint64_t> result;
    for(size_t i=4;i+16<=bytes.size();++i)if(std::memcmp(bytes.data()+i,"tfdt",4)==0){
        auto count=bytes[i+4]==1?8u:4u;if(i+8+count>bytes.size())throw std::runtime_error("tfdt bound");
        uint64_t time=0;for(size_t j=0;j<count;++j)time=(time<<8)|bytes[i+8+j];result.push_back(time);}
    check(result.size()==2,"fMP4 has audio/video decode timelines");return result;}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3){int seconds=std::stoi(argv[2]);if(seconds<4||seconds>30)throw std::runtime_error("Generated fixture duration bound");
        fs::path root(argv[1]);fs::create_directories(root);fixture(root/L"source.mp4",seconds);mkv(root/L"source.mp4",root/L"source.mkv");std::cout<<"Generated fixture seconds: "<<seconds<<'\n';return 0;}
    if(argc<2)return 2;auto root=fs::absolute(argv[1])/wide(random_token().substr(0,12));fs::create_directories(root);auto start=std::chrono::steady_clock::now();fixture(root/L"source.mp4");mkv(root/L"source.mp4",root/L"source.mkv");
    FileService files(root);auto tracks=media_tracks(files.media_source("source.mkv"),[]{return false;});check(tracks.width==3840&&tracks.height==2160&&!tracks.hdr,"4K HEVC SDR source inspected");
    check(tracks.tracks.size()==2&&tracks.tracks[0].codec=="aac"&&tracks.tracks[1].codec=="ass","audio/ASS track list queried");
    auto encode_start=std::chrono::steady_clock::now();auto plain=media_segment(files.media_source("source.mkv"),tracks.source.version,{1280,720,1,-1},0,[]{return false;});double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-encode_start).count();write_file(root/L"plain.mp4",plain);
    auto burned=media_segment(files.media_source("source.mkv"),tracks.source.version,{1280,720,1,2},0,[]{return false;});write_file(root/L"burned.mp4",burned);
    auto first=decoded_luma(root/L"plain.mp4"),second=decoded_luma(root/L"burned.mp4");uint64_t changed=0;for(size_t i=first.size()*3/4;i<first.size();i++)changed+=uint64_t(std::abs(int(first[i])-int(second[i])));check(changed>10000,"libass subtitle pixels burned into adapted video");
    auto seek=media_segment(files.media_source("source.mkv"),tracks.source.version,{1280,720,1,2},1,[]{return false;});write_file(root/L"seek.mp4",seek);decoded_luma(root/L"seek.mp4");
    auto tail=media_segment(files.media_source("source.mkv"),tracks.source.version,{1280,720,1,2},2,[]{return false;});write_file(root/L"tail.mp4",tail);decoded_luma(root/L"tail.mp4");
    auto a=decode_times(plain),b=decode_times(seek),c=decode_times(tail);
    check(a[0]<b[0]&&b[0]<c[0]&&a[1]<b[1]&&b[1]<c[1],"independent fMP4 segments retain increasing global decode times");
    auto audio=media_tracks(files.media_source("burned.mp4"),[]{return false;});check(audio.tracks.size()==1&&audio.tracks[0].codec=="aac","adapted segment contains AAC track");
    bool changed_source=false;try{media_segment(files.media_source("source.mkv"),"invalid",{1280,720,1,-1},0,[]{return false;});}catch(const Error& e){changed_source=e.code==Code::source_changed;}check(changed_source,"source version conflict preserves original");
    bool cancelled=false;try{media_segment(files.media_source("source.mkv"),tracks.source.version,{1280,720,1,-1},0,[]{return true;});}catch(const Error& e){cancelled=e.code==Code::cancelled;}check(cancelled,"segment cancellation");
    std::ofstream report(root/L"media-transcode.json");report<<"{\"kind\":\"measured\",\"checks\":"<<checks<<",\"source_4k_hevc10_sdr\":true,\"nvdec_cuda_nvenc\":true,\"ass_burn_pixel_difference\":"<<changed<<",\"first_2s_segment_encode_seconds\":"<<seconds<<",\"first_segment_bytes\":"<<plain.size()<<",\"original_bytes\":"<<fs::file_size(root/L"source.mkv")<<",\"optical_playback_tested\":false}";
    std::cout<<"Media transcode checks passed: "<<checks<<"; 2s segment "<<seconds<<"s; generated fixture "<<utf8(root.wstring())<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
