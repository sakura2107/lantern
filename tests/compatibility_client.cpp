#include "lantern/desktop_connection.hpp"
#include "lantern/audio_connection.hpp"
#include "lantern/windows_input_channel.hpp"
#include <iostream>
using namespace lantern;
int wmain(int argc,wchar_t** argv){try{
    if(argc!=4)return 2;auto bytes=unprotect(read_file(argv[3]));
    ClientCredentials c{"127.0.0.1",utf8(argv[2]),std::string(bytes.begin(),bytes.end()),uint16_t(std::stoi(argv[1]))};
    DesktopConnection desktop(c,0,{3840,120,false},{3840,2160,120,true});desktop.ready();auto frame=desktop.next();
    if(frame.config.codec!="h264"||frame.status.path!="tcp"||frame.config.width>1920||frame.config.fps>60||frame.frame.annex_b.empty())throw std::runtime_error("Old host H264/TCP fallback");desktop.interrupt();
    AudioConnection audio(c,true);audio.ready();auto packet=audio.next();if(audio.path()!="tcp"||!packet.pts_us)throw std::runtime_error("Old host audio TCP fallback");audio.interrupt();
    auto input=open_input_channel(c);input->submit(Op::input_focus,{});
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(input->acknowledged()<1&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if(input->acknowledged()!=1)throw std::runtime_error("Old host input RPC fallback");input->close();
    std::cout<<"New Windows backend -> old host H264/TCP video, audio and input passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
