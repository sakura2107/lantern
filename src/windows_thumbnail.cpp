#include "lantern/windows_thumbnail.hpp"
#include <wincodec.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace lantern {
PixelImage decode_jpeg(const Bytes& bytes, uint32_t max_pixels) {
    if (bytes.empty() || bytes.size() > max_frame)
        throw Error(Code::invalid, "JPEG payload bound");
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        throw Error(Code::unsupported, "WIC unavailable");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(f->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), DWORD(bytes.size()))) ||
        FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        throw Error(Code::invalid, "Invalid JPEG");
    GUID format{};
    decoder->GetContainerFormat(&format);
    if (format != GUID_ContainerFormatJpeg)
        throw Error(Code::invalid, "Expected JPEG");
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h || uint64_t(w) * h > max_pixels)
        throw Error(Code::invalid, "JPEG dimensions bound");
    if (FAILED(f->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom)))
        throw Error(Code::invalid, "JPEG conversion");
    PixelImage image{{}, int(w), int(h)};
    image.pixels.resize(size_t(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, UINT(image.pixels.size()), image.pixels.data())))
        throw Error(Code::invalid, "JPEG pixels");
    return image;
}
PixelImage client_thumbnail(Client& client, const std::string& path) {
    auto before = client.stat(path, false);
    Writer w;
    w.u32(1);
    w.str(path);
    w.u32(128);
    auto data = client.call(Op::thumbnail, std::move(w));
    Reader r{data};
    if (r.u32() != 1)
        throw Error(Code::invalid, "Thumbnail API version");
    auto jpeg = r.blob(262144);
    r.end();
    auto after = client.stat(path, false);
    if (before.version != after.version)
        throw Error(Code::conflict, "文件已变化，请重新选择");
    auto image = decode_jpeg(jpeg, 256 * 256);
    if (image.w > 256 || image.h > 256)
        throw Error(Code::invalid, "Thumbnail dimensions");
    return image;
}
struct ThumbnailLoader::Impl {
    std::mutex mutex;
    std::condition_variable cv;
    bool closing = false, pending = false;
    uint64_t epoch = 0, ticket = 0;
    ClientCredentials credentials;
    std::string path;
    ActiveConnection connection;
    std::thread worker;
    std::function<void(ThumbnailResult)> result;
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            closing = true;
            ++epoch;
            connection.interrupt();
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            ClientCredentials c;
            std::string p;
            uint64_t e, t;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return closing || pending; });
                if (closing)
                    break;
                c = credentials;
                p = path;
                t = ticket;
                e = epoch;
                pending = false;
            }
            ThumbnailResult output;
            output.ticket = t;
            try {
                auto client = c.connect(files_read, 0);
                connection.set(client);
                {
                    std::lock_guard lock(mutex);
                    if (closing || epoch != e)
                        continue;
                }
                output.image = client_thumbnail(*client, p);
                output.message = L"图片预览 · 128px";
            } catch (const std::exception& error) {
                output.message = L"预览不可用：" + wide(error.what());
            }
            connection.set({});
            bool deliver;
            {
                std::lock_guard lock(mutex);
                deliver = !closing && epoch == e;
            }
            if (deliver)
                result(std::move(output));
        }
        CoUninitialize();
    }
};
ThumbnailLoader::ThumbnailLoader(std::function<void(ThumbnailResult)> result) : p_(std::make_unique<Impl>()) {
    p_->result = std::move(result);
    p_->worker = std::thread([this] { p_->loop(); });
}
ThumbnailLoader::~ThumbnailLoader() = default;
void ThumbnailLoader::request(ClientCredentials c, std::string path, uint64_t ticket) {
    std::lock_guard lock(p_->mutex);
    p_->credentials = std::move(c);
    p_->path = std::move(path);
    p_->ticket = ticket;
    ++p_->epoch;
    p_->pending = true;
    p_->connection.interrupt();
    p_->cv.notify_all();
}
void ThumbnailLoader::cancel() {
    std::lock_guard lock(p_->mutex);
    ++p_->epoch;
    p_->pending = false;
    p_->connection.interrupt();
    p_->cv.notify_all();
}
} // namespace lantern
