#include "lantern/win.hpp"
#include <wincodec.h>
#include <wrl/client.h>
namespace lantern {
using Microsoft::WRL::ComPtr;
static BOOL CALLBACK enumerate(HMONITOR h, HDC, LPRECT, LPARAM context) {
    MONITORINFOEXW i{};
    i.cbSize = sizeof(i);
    if (GetMonitorInfoW(h, &i)) {
        auto& r = i.rcMonitor;
        ((std::vector<Monitor>*)context)
            ->push_back({r.left, r.top, r.right - r.left, r.bottom - r.top, utf8(i.szDevice)});
    }
    return TRUE;
}
std::vector<Monitor> monitors() {
    std::vector<Monitor> v;
    EnumDisplayMonitors(nullptr, nullptr, enumerate, (LPARAM)&v);
    return v;
}
static void hr(HRESULT h) {
    if (FAILED(h))
        throw Error(Code::io, "Desktop encoder HRESULT=" + std::to_string(uint32_t(h)));
}
Bytes capture(uint32_t index, uint32_t target, uint32_t quality) {
    auto v = monitors();
    if (index >= v.size() || target < 160 || target > 3840 || quality < 20 || quality > 95)
        throw Error(Code::invalid, "Capture parameters");
    auto m = v[index];
    int width = std::min<int>(m.width, target), height = std::max(1, m.height * width / m.width);
    if (uint64_t(width) * height > 16000000)
        throw Error(Code::invalid, "Capture pixel bound");
    HDC screen = GetDC(nullptr);
    if (!screen)
        throw Error(Code::unsupported, "Interactive desktop unavailable");
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        if (dc)
            DeleteDC(dc);
        if (bitmap)
            DeleteObject(bitmap);
        ReleaseDC(nullptr, screen);
        throw Error(Code::io, "Capture surface allocation");
    }
    auto old = SelectObject(dc, bitmap);
    SetStretchBltMode(dc, HALFTONE);
    BOOL copied =
        StretchBlt(dc, 0, 0, width, height, screen, m.x, m.y, m.width, m.height, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    Bytes raw(size_t(width) * height * 4);
    if (copied)
        std::copy_n((uint8_t*)pixels, raw.size(), raw.data());
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    if (!copied)
        throw Error(Code::unsupported, "Capture failed; locked or protected desktop may be unavailable");
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Co {
        HRESULT h;
        ~Co() {
            if (SUCCEEDED(h))
                CoUninitialize();
        }
    } cleanup{co};
    ComPtr<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IStream> stream;
    hr(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder;
    hr(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr(encoder->CreateNewFrame(&frame, &props));
    PROPBAG2 opt{};
    opt.pstrName = const_cast<wchar_t*>(L"ImageQuality");
    VARIANT val{};
    val.vt = VT_R4;
    val.fltVal = quality / 100.f;
    hr(props->Write(1, &opt, &val));
    hr(frame->Initialize(props.Get()));
    hr(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    hr(frame->SetPixelFormat(&format));
    Bytes bgr(size_t(width) * height * 3);
    for (size_t i = 0, j = 0; i < raw.size(); i += 4, j += 3) {
        bgr[j] = raw[i];
        bgr[j + 1] = raw[i + 1];
        bgr[j + 2] = raw[i + 2];
    }
    hr(frame->WritePixels(height, width * 3, UINT(bgr.size()), bgr.data()));
    hr(frame->Commit());
    hr(encoder->Commit());
    STATSTG st{};
    hr(stream->Stat(&st, STATFLAG_NONAME));
    if (st.cbSize.QuadPart > max_frame - 128)
        throw Error(Code::busy, "JPEG exceeds frame bound; lower quality or resolution");
    Bytes out(size_t(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    hr(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
    ULONG n;
    hr(stream->Read(out.data(), ULONG(out.size()), &n));
    return out;
}
} // namespace lantern
