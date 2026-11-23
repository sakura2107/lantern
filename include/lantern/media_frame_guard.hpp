#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace lantern {
struct AudioInputFormat {
    int sample_rate{}, sample_format{}, channels{}, layout_order{};
    std::uint64_t channel_mask{};
    std::array<int, 16> custom_channels{}; // Speaker IDs in input order for a custom layout.
    bool planar{};
    int required_planes{}, bytes_per_sample{};
    bool operator==(const AudioInputFormat&) const = default;
};

inline constexpr bool valid_audio_frame(const AudioInputFormat& expected, const AudioInputFormat& actual,
                                        std::span<const std::uint8_t* const> planes,
                                        int nb_samples, std::int64_t skip) noexcept {
    if (actual != expected || actual.sample_rate < 8000 || actual.sample_rate > 192000 ||
        actual.channels < 1 || actual.channels > 16 || actual.sample_format < 0 ||
        actual.bytes_per_sample < 1 || actual.bytes_per_sample > 8 ||
        actual.required_planes != (actual.planar ? actual.channels : 1) ||
        planes.size() < static_cast<std::size_t>(actual.required_planes) ||
        nb_samples < 0 || nb_samples > (1 << 20) || skip < 0 || skip > nb_samples)
        return false;
    for (int i = 0; i < actual.required_planes; ++i)
        if (!planes[i])
            return false;
    return true;
}
} // namespace lantern
