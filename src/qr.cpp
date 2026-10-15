#include "lantern/pairing.hpp"
#include "qrcodegen.hpp"
namespace lantern {
QrMatrix qr_encode(std::string_view text) {
    if (text.empty() || text.size() > 8192 || text.find('\0') != text.npos)
        throw Error(Code::invalid, "二维码内容为空或过长");
    try {
        auto segments = qrcodegen::QrSegment::makeSegments(std::string(text).c_str());
        auto qr =
            qrcodegen::QrCode::encodeSegments(segments, qrcodegen::QrCode::Ecc::MEDIUM, 1, 40, -1, false);
        QrMatrix result;
        result.size = qr.getSize();
        result.modules.reserve(size_t(result.size) * result.size);
        for (int y = 0; y < result.size; ++y)
            for (int x = 0; x < result.size; ++x)
                result.modules.push_back(qr.getModule(x, y));
        return result;
    } catch (const qrcodegen::data_too_long&) {
        throw Error(Code::invalid, "二维码内容过长，请减少地址或名称长度");
    }
}
} // namespace lantern
