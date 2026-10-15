#pragma once
#include "windows_client.hpp"
namespace lantern {
struct PixelImage {
    Bytes pixels;
    int w = 0, h = 0;
};
PixelImage decode_jpeg(const Bytes&, uint32_t max_pixels = 16000000);
PixelImage client_thumbnail(Client&, const std::string&);
struct ThumbnailResult {
    uint64_t ticket = 0;
    PixelImage image;
    std::wstring message;
};
// One active request and one replacement; results carry a UI generation, never cache file contents.
class ThumbnailLoader {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    explicit ThumbnailLoader(std::function<void(ThumbnailResult)>);
    ~ThumbnailLoader();
    void request(ClientCredentials, std::string, uint64_t);
    void cancel();
};
} // namespace lantern
