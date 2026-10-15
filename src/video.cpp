#include "lantern/win.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <nvEncodeAPI.h>
#include <wrl/client.h>
namespace lantern {
using Microsoft::WRL::ComPtr;
namespace {
void gpu(HRESULT value, const char* where) {
    if (FAILED(value))
        throw Error(Code::unsupported, std::string(where) + " HRESULT=" + std::to_string(uint32_t(value)));
}
void enc(NVENCSTATUS value, const char* where) {
    if (value != NV_ENC_SUCCESS)
        throw Error(Code::unsupported, std::string(where) + " NVENC=" + std::to_string(value));
}
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
class NvDesktop final : public DesktopStream {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> cached, target;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    HMODULE library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* encoder = nullptr;
    NV_ENC_REGISTERED_PTR registered = nullptr;
    NV_ENC_OUTPUT_PTR output = nullptr;
    bool have_surface = false;
    VideoConfig configuration{};
    uint64_t sequence = 0;
    Clock::time_point start = Clock::now();

  public:
    ~NvDesktop() override {
        if (encoder) {
            if (registered)
                api.nvEncUnregisterResource(encoder, registered);
            if (output)
                api.nvEncDestroyBitstreamBuffer(encoder, output);
            api.nvEncDestroyEncoder(encoder);
        }
        if (library)
            FreeLibrary(library);
    }
    void init(uint32_t index, uint32_t requested_width, uint32_t fps, uint32_t bitrate) {
        if (requested_width < 640 || requested_width > 1920 || (fps != 30 && fps != 60) ||
            bitrate < 2'000'000 || bitrate > 30'000'000)
            throw Error(Code::invalid, "H264 v1 accepts width 640..1920, 30/60 fps, 2..30 Mbps");
        auto displays = monitors();
        if (index >= displays.size())
            throw Error(Code::invalid, "Monitor index");
        auto wanted = displays[index];
        ComPtr<IDXGIFactory1> factory;
        gpu(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> chosen;
        ComPtr<IDXGIOutput> selected;
        for (UINT a = 0; !selected; a++) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            for (UINT o = 0;; o++) {
                ComPtr<IDXGIOutput> candidate;
                if (adapter->EnumOutputs(o, &candidate) == DXGI_ERROR_NOT_FOUND)
                    break;
                DXGI_OUTPUT_DESC desc{};
                gpu(candidate->GetDesc(&desc), "Output description");
                if (utf8(desc.DeviceName) == wanted.name && desc.AttachedToDesktop) {
                    chosen = adapter;
                    selected = candidate;
                    break;
                }
            }
        }
        if (!selected)
            throw Error(Code::unsupported, "Display is unavailable to DXGI");
        DXGI_ADAPTER_DESC1 adapter_desc{};
        chosen->GetDesc1(&adapter_desc);
        if (adapter_desc.VendorId != 0x10de)
            throw Error(Code::unsupported,
                        "NVENC provider requires an NVIDIA GPU driving this monitor; choose JPEG");
        D3D_FEATURE_LEVEL feature;
        gpu(D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr,
                              0, D3D11_SDK_VERSION, &device, &feature, &context),
            "D3D11 hardware device");
        ComPtr<IDXGIOutput1> output1;
        gpu(selected.As(&output1), "Desktop Duplication output");
        gpu(output1->DuplicateOutput(device.Get(), &duplication), "Desktop Duplication");
        DXGI_OUTDUPL_DESC desktop_desc{};
        duplication->GetDesc(&desktop_desc);
        if (desktop_desc.Rotation != DXGI_MODE_ROTATION_IDENTITY ||
            desktop_desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            throw Error(Code::unsupported,
                        "Rotated or non-BGRA desktop is unsupported by H264 v1; choose JPEG");
        uint32_t source_width = desktop_desc.ModeDesc.Width, source_height = desktop_desc.ModeDesc.Height;
        configuration.width = std::min(source_width, requested_width) & ~1u;
        configuration.height = (uint32_t(uint64_t(source_height) * configuration.width / source_width) & ~1u);
        if (!configuration.height || uint64_t(configuration.width) * configuration.height > 4'000'000)
            throw Error(Code::unsupported, "H264 desktop pixel bound");
        configuration.fps = fps;
        configuration.bitrate = bitrate;
        configuration.capture_provider = "dxgi-d3d11";
        configuration.encoder_provider = "nvidia-nvenc-h264";
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = source_width;
        desc.Height = source_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        gpu(device->CreateTexture2D(&desc, nullptr, &cached), "GPU capture cache");
        desc.Width = configuration.width;
        desc.Height = configuration.height;
        gpu(device->CreateTexture2D(&desc, nullptr, &target), "GPU encoder input");
        gpu(device.As(&video_device), "D3D11 video device");
        gpu(context.As(&video_context), "D3D11 video context");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {fps, 1};
        content.OutputFrameRate = {fps, 1};
        content.InputWidth = source_width;
        content.InputHeight = source_height;
        content.OutputWidth = configuration.width;
        content.OutputHeight = configuration.height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        gpu(video_device->CreateVideoProcessorEnumerator(&content, &enumerator), "GPU scaler enumerator");
        gpu(video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor), "GPU scaler");
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in{};
        in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        gpu(video_device->CreateVideoProcessorInputView(cached.Get(), enumerator.Get(), &in, &input_view),
            "GPU scaler input");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{};
        out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        gpu(video_device->CreateVideoProcessorOutputView(target.Get(), enumerator.Get(), &out, &output_view),
            "GPU scaler output");
        RECT source_rect{0, 0, LONG(source_width), LONG(source_height)},
            dest_rect{0, 0, LONG(configuration.width), LONG(configuration.height)};
        video_context->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &source_rect);
        video_context->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dest_rect);
        video_context->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &dest_rect);
        video_context->VideoProcessorSetStreamFrameFormat(processor.Get(), 0,
                                                          D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        library = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            throw Error(Code::unsupported, "NVIDIA driver encoder library unavailable; choose JPEG");
        auto max_version = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(uint32_t*)>(
            GetProcAddress(library, "NvEncodeAPIGetMaxSupportedVersion"));
        auto create = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(
            GetProcAddress(library, "NvEncodeAPICreateInstance"));
        if (!max_version || !create)
            throw Error(Code::unsupported, "NVENC ABI entry points missing");
        uint32_t version = 0;
        enc(max_version(&version), "NVENC driver version");
        if (version < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION))
            throw Error(Code::unsupported, "NVIDIA driver is older than NVENC API 12.2");
        api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        enc(create(&api), "NVENC function table");
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.device = device.Get();
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.apiVersion = NVENCAPI_VERSION;
        enc(api.nvEncOpenEncodeSessionEx(&open, &encoder), "NVENC hardware session");
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        enc(api.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
                                             NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset),
            "NVENC low latency preset");
        auto& cfg = preset.presetCfg;
        cfg.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        cfg.gopLength = fps * 2;
        cfg.frameIntervalP = 1;
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        cfg.rcParams.averageBitRate = bitrate;
        cfg.rcParams.maxBitRate = bitrate;
        cfg.rcParams.vbvBufferSize = bitrate / fps;
        cfg.rcParams.vbvInitialDelay = bitrate / fps;
        cfg.rcParams.enableLookahead = 0;
        cfg.rcParams.zeroReorderDelay = 1;
        cfg.encodeCodecConfig.h264Config.idrPeriod = fps * 2;
        cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = NV_ENC_CODEC_H264_GUID;
        init.presetGUID = NV_ENC_PRESET_P1_GUID;
        init.encodeWidth = configuration.width;
        init.encodeHeight = configuration.height;
        init.darWidth = configuration.width;
        init.darHeight = configuration.height;
        init.frameRateNum = fps;
        init.frameRateDen = 1;
        init.enablePTD = 1;
        init.enableEncodeAsync = 0;
        init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        init.encodeConfig = &cfg;
        enc(api.nvEncInitializeEncoder(encoder, &init), "NVENC initialize H264");
        NV_ENC_REGISTER_RESOURCE resource{};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.resourceToRegister = target.Get();
        resource.width = configuration.width;
        resource.height = configuration.height;
        resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        resource.bufferUsage = NV_ENC_INPUT_IMAGE;
        enc(api.nvEncRegisterResource(encoder, &resource), "NVENC register GPU surface");
        registered = resource.registeredResource;
        NV_ENC_CREATE_BITSTREAM_BUFFER buffer{};
        buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        enc(api.nvEncCreateBitstreamBuffer(encoder, &buffer), "NVENC output buffer");
        output = buffer.bitstreamBuffer;
        Bytes headers(4096);
        uint32_t header_size = 0;
        NV_ENC_SEQUENCE_PARAM_PAYLOAD sequence_params{};
        sequence_params.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
        sequence_params.spsppsBuffer = headers.data();
        sequence_params.inBufferSize = uint32_t(headers.size());
        sequence_params.outSPSPPSPayloadSize = &header_size;
        enc(api.nvEncGetSequenceParams(encoder, &sequence_params), "H264 SPS/PPS");
        if (!header_size || header_size > headers.size())
            throw Error(Code::io, "H264 headers bound");
        headers.resize(header_size);
        configuration.headers = std::move(headers);
        start = Clock::now();
    }
    const VideoConfig& config() const override {
        return configuration;
    }
    VideoFrame next() override {
        auto began = Clock::now();
        ComPtr<IDXGIResource> desktop;
        DXGI_OUTDUPL_FRAME_INFO info{};
        HRESULT result = duplication->AcquireNextFrame(have_surface ? 0 : 100, &info, &desktop);
        bool repeated = result == DXGI_ERROR_WAIT_TIMEOUT;
        if (!repeated) {
            gpu(result, "Desktop acquisition (unlock/restart after display or session change)");
            struct Release {
                IDXGIOutputDuplication* p;
                ~Release() {
                    p->ReleaseFrame();
                }
            } release{duplication.Get()};
            ComPtr<ID3D11Texture2D> image;
            gpu(desktop.As(&image), "Desktop GPU texture");
            context->CopyResource(cached.Get(), image.Get());
            have_surface = true;
        }
        if (!have_surface)
            throw Error(Code::busy, "No desktop surface available");
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view.Get();
        gpu(video_context->VideoProcessorBlt(processor.Get(), output_view.Get(), UINT(sequence), 1, &stream),
            "GPU desktop scaling");
        VideoFrame frame{};
        frame.capture_ms = ms(began);
        frame.clock_us = monotonic_us();
        frame.sequence = sequence;
        frame.pts_us =
            uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        auto encode_start = Clock::now();
        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registered;
        enc(api.nvEncMapInputResource(encoder, &map), "NVENC map surface");
        struct Unmap {
            NV_ENCODE_API_FUNCTION_LIST& api;
            void* e;
            NV_ENC_INPUT_PTR input;
            ~Unmap() {
                api.nvEncUnmapInputResource(e, input);
            }
        } unmap{api, encoder, map.mappedResource};
        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = map.mappedBufferFmt;
        pic.inputWidth = configuration.width;
        pic.inputHeight = configuration.height;
        pic.outputBitstream = output;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = frame.pts_us;
        if (sequence == 0)
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        enc(api.nvEncEncodePicture(encoder, &pic), "NVENC encode");
        NV_ENC_LOCK_BITSTREAM locked{};
        locked.version = NV_ENC_LOCK_BITSTREAM_VER;
        locked.outputBitstream = output;
        enc(api.nvEncLockBitstream(encoder, &locked), "NVENC bitstream ready");
        struct Unlock {
            NV_ENCODE_API_FUNCTION_LIST& api;
            void* e;
            NV_ENC_OUTPUT_PTR output;
            ~Unlock() {
                api.nvEncUnlockBitstream(e, output);
            }
        } unlock{api, encoder, output};
        if (locked.bitstreamSizeInBytes > max_frame - 256)
            throw Error(Code::busy, "Encoded desktop frame exceeds bound");
        auto bytes = static_cast<uint8_t*>(locked.bitstreamBufferPtr);
        frame.annex_b.assign(bytes, bytes + locked.bitstreamSizeInBytes);
        frame.flags = ((locked.pictureType == NV_ENC_PIC_TYPE_IDR) ? 1u : 0u) | (repeated ? 2u : 0u);
        frame.encode_ms = ms(encode_start);
        ++sequence;
        return frame;
    }
};
} // namespace
std::unique_ptr<DesktopStream> open_desktop_stream(uint32_t monitor, uint32_t width, uint32_t fps,
                                                   uint32_t bitrate) {
    auto stream = std::make_unique<NvDesktop>();
    stream->init(monitor, width, fps, bitrate);
    return stream;
}
} // namespace lantern
