#include "lantern/win.hpp"
#include <wincodec.h>
#include <wrl/client.h>
namespace lantern {
using Microsoft::WRL::ComPtr;
static void preview_hr(HRESULT result) {
    if (FAILED(result))
        throw Error(Code::unsupported, "Image preview decoder/encoder rejected input");
}
Bytes image_thumbnail(std::span<const uint8_t> input, uint32_t width) {
    if (width < 32 || width > 256 || input.empty() || input.size() > 16 * 1024 * 1024)
        throw Error(Code::invalid, "Thumbnail size bound");
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init))
        throw Error(Code::unsupported, "Preview COM initialization");
    struct Uninit {
        ~Uninit() {
            CoUninitialize();
        }
    } uninit;
    ComPtr<IWICImagingFactory> factory;
    preview_hr(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    preview_hr(factory->CreateStream(&stream));
    preview_hr(stream->InitializeFromMemory(const_cast<BYTE*>(input.data()), DWORD(input.size())));
    GUID container{};
    if (input.size() >= 2 && input[0] == 0xff && input[1] == 0xd8)
        container = GUID_ContainerFormatJpeg;
    else if (input.size() >= 8 && input[0] == 137 && input[1] == 80 && input[2] == 78 && input[3] == 71)
        container = GUID_ContainerFormatPng;
    else if (input.size() >= 2 && input[0] == 'B' && input[1] == 'M')
        container = GUID_ContainerFormatBmp;
    else
        throw Error(Code::unsupported, "Only JPEG, PNG and BMP thumbnails are supported");
    ComPtr<IWICBitmapDecoder> decoder;
    preview_hr(factory->CreateDecoder(container, &GUID_VendorMicrosoft, &decoder));
    preview_hr(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand));
    ComPtr<IWICBitmapFrameDecode> image;
    preview_hr(decoder->GetFrame(0, &image));
    UINT w = 0, h = 0;
    preview_hr(image->GetSize(&w, &h));
    if (!w || !h || w > 32000 || h > 32000 || uint64_t(w) * h > 64000000)
        throw Error(Code::unsupported, "Image dimensions exceed preview limit");
    UINT outw = w >= h ? std::min(w, width) : std::max(1u, UINT(uint64_t(w) * std::min(h, width) / h));
    UINT outh = w >= h ? std::max(1u, UINT(uint64_t(h) * outw / w)) : std::min(h, width);
    ComPtr<IWICBitmapScaler> scaler;
    preview_hr(factory->CreateBitmapScaler(&scaler));
    preview_hr(scaler->Initialize(image.Get(), outw, outh, WICBitmapInterpolationModeFant));
    ComPtr<IWICFormatConverter> converter;
    preview_hr(factory->CreateFormatConverter(&converter));
    preview_hr(converter->Initialize(scaler.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom));
    Bytes pixels(outw * outh * 3);
    preview_hr(converter->CopyPixels(nullptr, outw * 3, UINT(pixels.size()), pixels.data()));
    ComPtr<IStream> output;
    preview_hr(CreateStreamOnHGlobal(nullptr, TRUE, &output));
    ComPtr<IWICBitmapEncoder> encoder;
    preview_hr(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
    preview_hr(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    preview_hr(encoder->CreateNewFrame(&frame, &options));
    preview_hr(frame->Initialize(options.Get()));
    preview_hr(frame->SetSize(outw, outh));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    preview_hr(frame->SetPixelFormat(&format));
    preview_hr(frame->WritePixels(outh, outw * 3, UINT(pixels.size()), pixels.data()));
    preview_hr(frame->Commit());
    preview_hr(encoder->Commit());
    STATSTG size{};
    preview_hr(output->Stat(&size, STATFLAG_NONAME));
    if (size.cbSize.QuadPart > 256 * 1024)
        throw Error(Code::busy, "Thumbnail encoded bound");
    Bytes jpeg(size_t(size.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    preview_hr(output->Seek(zero, STREAM_SEEK_SET, nullptr));
    ULONG got = 0;
    preview_hr(output->Read(jpeg.data(), ULONG(jpeg.size()), &got));
    if (got != jpeg.size())
        throw Error(Code::io, "Thumbnail short read");
    return jpeg;
}
} // namespace lantern
