#include "lantern/windows_playback.hpp"
#include "lantern/quality.hpp"
#include <fstream>
#include <iostream>
#include <objbase.h>
using namespace lantern;
int wmain(int argc,wchar_t** argv){HWND window=nullptr;bool com=false;try{
    if(argc!=2)return 2;
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))throw std::runtime_error("COM MTA");com=true;
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"LanternOwnedGpuValidation";wc.hbrBackground=CreateSolidBrush(RGB(30,100,80));RegisterClassW(&wc);
    // Cover this monitor with owned generated pixels; no desktop capture is saved.
    window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"Lantern generated GPU validation",WS_POPUP|WS_VISIBLE,
        0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),nullptr,nullptr,wc.hInstance,nullptr);
    if(!window)throw std::runtime_error("Owned test window");UpdateWindow(window);
    std::ofstream report{fs::path(argv[1])};report<<"{\"measured\":true,\"native_4k_display\":false,\"hevc_decoder_available\":"<<(WindowsVideo::supports_hevc()?"true":"false")<<",\"matrix\":[";
    bool comma=false;int configurations=0;bool hevc=WindowsVideo::supports_hevc();
    for(bool request_hevc:{false,true}){
        if(request_hevc&&!hevc)continue;
        auto encoder=open_desktop_stream(0,1280,30,5'000'000,request_hevc,true);
        if(encoder->config().codec!=(request_hevc?"hevc":"h264"))throw std::runtime_error("Requested encoder unavailable");
        for(auto width:{1280u,1920u,2560u,3840u})for(auto fps:{30u,60u,120u}){
            encoder->reconfigure(width,fps,AdaptiveQuality::target(width,fps));WindowsVideo video(window,encoder->config());
            auto start=std::chrono::steady_clock::now();
            for(int frame=0;frame<16;++frame){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                auto packet=encoder->next();video.frame(packet.annex_b,packet.pts_us,true);}
            auto stats=video.stats();if(!stats.presented||stats.gpu_surfaces!=stats.decoded||stats.cpu_surfaces)throw std::runtime_error("GPU presentation was not verified");
            if(comma)report<<',';comma=true;++configurations;
            report<<"{\"width\":"<<width<<",\"fps\":"<<fps<<",\"codec\":\""<<encoder->config().codec<<"\",\"decoded\":"<<stats.decoded<<",\"presented\":"<<stats.presented<<",\"gpu_surfaces\":"<<stats.gpu_surfaces<<",\"elapsed_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<'}';report.flush();
        }
    }
    report<<"],\"configurations\":"<<configurations<<",\"sustained_fps_measured\":false,\"optical_latency_measured\":false}";
    DestroyWindow(window);DeleteObject(wc.hbrBackground);CoUninitialize();
    std::cout<<"Windows GPU decode/present configurations passed: "<<configurations<<"; HEVC system decoder "<<(hevc?"available":"unavailable (H264 fallback)")<<'\n';return 0;
}catch(const std::exception& e){if(window)DestroyWindow(window);if(com)CoUninitialize();std::cerr<<e.what()<<'\n';return 1;}}
