#include "lantern/av_timeline.hpp"
#include "lantern/core.hpp"
#include "lantern/media_frame_guard.hpp"
#include "lantern/plugin.h"
#include <array>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace lantern;
static int checks = 0;
void check(bool b, const char* msg) {
    ++checks;
    if (!b)
        throw std::runtime_error(msg);
}
int main(int argc, char** argv) {
    try {
        AvTimeline timeline;
        uint64_t clock_now = 2'000'000'000, host = 5'000'000;
        check(timeline.deadline(host, clock_now) == int64_t(clock_now), "video only immediate");
        timeline.synchronize(true);
        auto owner = timeline.begin_audio();
        timeline.anchor(owner, host, clock_now);
        check(timeline.deadline(host + 60'000, clock_now) == int64_t(clock_now + 60'000'000),
              "audio hardware master");
        check(timeline.deadline(host - 90'000, clock_now) == -1, "late video dropped");
        check(timeline.deadline(host + 400'000, clock_now) == int64_t(clock_now), "future bounded");
        check(timeline.deadline(host, clock_now + 600'000'000) == int64_t(clock_now + 600'000'000),
              "stale clock no stall");
        auto newer = timeline.begin_audio();
        timeline.end_audio(owner);
        timeline.anchor(owner, host, clock_now);
        check(timeline.deadline(host + 60'000, clock_now) == int64_t(clock_now), "old audio owner rejected");
        timeline.anchor(newer, host, clock_now);
        for (int i = 0; i < 1000; i++)
            timeline.observe(host, clock_now);
        check(timeline.samples().size() == 512, "bounded clock observations");
        timeline.end_audio(newer);
        for (auto input : {"pc.local:47443", "[::1]:47443", "[fe80::1%3]:443", "::1"}) {
            auto e = parse_endpoint(input);
            check(parse_endpoint(format_endpoint(e)).host == e.host, "endpoint round trip");
        }
        for (auto input : {"", "a:0", "a:65536", "a:-1", "[::1]:", "[::1]junk", "a/b", "x@y", "a b"}) {
            bool rejected = false;
            try {
                parse_endpoint(input);
            } catch (const Error&) {
                rejected = true;
            }
            check(rejected, "invalid endpoint");
        }
        Writer w;
        w.u16(65535);
        w.u32(0xffffffff);
        w.u64(UINT64_MAX);
        w.str("字幕 / test");
        Reader r{w.b};
        check(r.u16() == 65535, "u16");
        check(r.u32() == 0xffffffff, "u32");
        check(r.u64() == UINT64_MAX, "u64");
        check(r.str() == "字幕 / test", "utf8 payload");
        r.end();
        for (size_t n = 0; n < w.b.size(); ++n) {
            Reader broken{{w.b.data(), n}};
            bool threw = false;
            try {
                broken.u16();
                broken.u32();
                broken.u64();
                broken.str();
                broken.end();
            } catch (const Error&) {
                threw = true;
            }
            check(threw, "truncation not rejected");
        }
        Writer huge;
        huge.u32(max_frame + 1);
        bool threw = false;
        try {
            Reader t{huge.b};
            t.blob();
        } catch (const Error&) {
            threw = true;
        }
        check(threw, "oversized blob");
        for (auto p : {"../a", "a/../b", "/root", "C:/x", "a\\b", "a//b", "a/", "NUL.txt", "con", "COM1",
                       "lpt9.log", "a:stream", "a. ", ".rdx-private/x", "a/.RDX-PRIVATE-copy"})
            check(!safe_path(p), p);
        for (auto p : {"", "Films/a.mkv", "字幕.ass", "a b/x.txt"})
            check(safe_path(p), p);
        check(parse_range("5-9", 100).begin == 5 && parse_range("5-9", 100).end == 9, "closed range");
        check(parse_range("5-", 100).end == 99, "open range");
        check(parse_range("-10", 100).begin == 90, "suffix range");
        check(parse_range("-1000", 100).begin == 0, "suffix clipped");
        check(parse_range("0-1000", 100).end == 99, "end clipped");
        for (auto value :
             {"", "-0", "x-5", "4-3", "100-", "1-2,3-4", "1-2abc", "18446744073709551616-", "+1-2", "--1"}) {
            bool rejected = false;
            try {
                parse_range(value, 100);
            } catch (const Error&) {
                rejected = true;
            }
            check(rejected, "invalid range accepted");
        }
        Budget b(1024 * 1024);
        auto now = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        check(b.admit(Lane::bulk, 256 * 1024, now), "bulk budget");
        check(!b.admit(Lane::bulk, 1, now), "bulk bound");
        check(b.admit(Lane::input, 1024, now), "input reserved");
        check(b.admit(Lane::media, 512 * 1024, now), "media independent");
        check(!b.admit(Lane::media, 1, now), "media bound");
        check(transition_allowed(TaskState::running, TaskState::paused), "pause");
        check(transition_allowed(TaskState::paused, TaskState::running), "resume");
        check(!transition_allowed(TaskState::running, TaskState::completed), "unverified complete");
        check(!transition_allowed(TaskState::completed, TaskState::running), "terminal ownership");
        {
            // WP-SEC-04 (BUG-02): deterministic audio frame format guard tests (FFmpeg-independent).
            AudioInputFormat expected{};
            expected.sample_rate = 48000;
            expected.sample_format = 8; // e.g., FLTP
            expected.channels = 2;
            expected.layout_order = 1; // native
            expected.channel_mask = 0x3; // stereo
            expected.planar = true;
            expected.required_planes = 2;
            expected.bytes_per_sample = 4;

            std::uint8_t plane0[16]{}, plane1[16]{};
            const std::uint8_t* valid_planes[2]{plane0, plane1};
            check(valid_audio_frame(expected, expected, valid_planes, 1024, 0),
                  "audio frame guard accepts matching planar format");
            check(valid_audio_frame(expected, expected, valid_planes, 1024, 512),
                  "audio frame guard accepts in-bounds skip");
            check(valid_audio_frame(expected, expected, valid_planes, 1024, 1024),
                  "audio frame guard accepts skip equal to nb_samples");
            check(valid_audio_frame(expected, expected, valid_planes, 0, 0),
                  "audio frame guard accepts zero-sample frame with zero skip");
            check(valid_audio_frame(expected, expected, valid_planes, 1 << 20, 0),
                  "audio frame guard accepts max nb_samples bound");

            AudioInputFormat packed = expected;
            packed.sample_format = 1; // e.g., S16
            packed.planar = false;
            packed.required_planes = 1;
            packed.bytes_per_sample = 2;
            const std::uint8_t* packed_plane[1]{plane0};
            check(valid_audio_frame(packed, packed, packed_plane, 1024, 1024),
                  "audio frame guard accepts matching packed format at skip == nb_samples");

            // Channel layout / format changes rejected.
            AudioInputFormat changed_channels = expected;
            changed_channels.channels = 1;
            changed_channels.channel_mask = 0x4;
            changed_channels.required_planes = 1;
            check(!valid_audio_frame(expected, changed_channels, valid_planes, 1024, 0),
                  "audio frame guard rejects channel count change");

            AudioInputFormat changed_mask = expected;
            changed_mask.channel_mask = 0x33;
            check(!valid_audio_frame(expected, changed_mask, valid_planes, 1024, 0),
                  "audio frame guard rejects channel mask change");

            AudioInputFormat changed_order = expected;
            changed_order.layout_order = 2;
            changed_order.custom_channels[0] = 1;
            check(!valid_audio_frame(expected, changed_order, valid_planes, 1024, 0),
                  "audio frame guard rejects layout order / custom channel change");

            AudioInputFormat changed_planar = expected;
            changed_planar.planar = false;
            changed_planar.required_planes = 1;
            check(!valid_audio_frame(expected, changed_planar, valid_planes, 1024, 0),
                  "audio frame guard rejects planar/packed change");

            AudioInputFormat changed_fmt = expected;
            changed_fmt.sample_format = 3;
            check(!valid_audio_frame(expected, changed_fmt, valid_planes, 1024, 0),
                  "audio frame guard rejects sample format change");

            // Missing or null planes rejected.
            const std::uint8_t* short_planes[1]{plane0};
            check(!valid_audio_frame(expected, expected, short_planes, 1024, 0),
                  "audio frame guard rejects missing planar plane");
            check(!valid_audio_frame(expected, expected, {}, 1024, 0),
                  "audio frame guard rejects empty plane span");
            const std::uint8_t* null_first[2]{nullptr, plane1};
            check(!valid_audio_frame(expected, expected, null_first, 1024, 0),
                  "audio frame guard rejects null first plane");
            const std::uint8_t* null_second[2]{plane0, nullptr};
            check(!valid_audio_frame(expected, expected, null_second, 1024, 0),
                  "audio frame guard rejects null second plane");
            AudioInputFormat bad_required = expected;
            bad_required.required_planes = 1;
            check(!valid_audio_frame(bad_required, bad_required, valid_planes, 1024, 0),
                  "audio frame guard rejects inconsistent required_planes");

            // Invalid sample rates rejected.
            for (int bad_rate : {-48000, 0, 7999, 192001}) {
                AudioInputFormat bad = expected;
                bad.sample_rate = bad_rate;
                check(!valid_audio_frame(bad, bad, valid_planes, 1024, 0),
                      "audio frame guard rejects invalid sample rate");
            }
            AudioInputFormat changed_rate = expected;
            changed_rate.sample_rate = 44100;
            check(!valid_audio_frame(expected, changed_rate, valid_planes, 1024, 0),
                  "audio frame guard rejects sample rate change");

            // Sample count and skip bounds.
            check(!valid_audio_frame(expected, expected, valid_planes, -1, 0),
                  "audio frame guard rejects negative nb_samples");
            check(!valid_audio_frame(expected, expected, valid_planes, (1 << 20) + 1, 0),
                  "audio frame guard rejects nb_samples above 1<<20");
            check(!valid_audio_frame(expected, expected, valid_planes, 1024, -1),
                  "audio frame guard rejects negative skip");
            check(!valid_audio_frame(expected, expected, valid_planes, 1024, 1025),
                  "audio frame guard rejects skip greater than nb_samples");
            check(!valid_audio_frame(expected, expected, valid_planes, 0, 1),
                  "audio frame guard rejects positive skip on empty frame");
        }
#ifdef _WIN32
        check(argc > 1, "plugin path");
        auto lib = LoadLibraryA(argv[1]);
        check(lib != nullptr, "load plugin");
        auto query = (lp_query_fn)GetProcAddress(lib, "lantern_plugin_query");
        check(query != nullptr, "plugin entry");
        lp_api_v1 api{};
        check(query(2, sizeof(api), &api) == 4, "ABI incompatibility");
        check(query(LP_ABI_V1, 1, &api) == 4, "structure bound");
        check(query(LP_ABI_V1, sizeof(api), &api) == 0, "ABI query");
        lp_handle h = nullptr;
        check(api.create(&h) == 0, "provider create");
        uint32_t n = 0;
        uint8_t input[] = {4, 5, 6}, output[3];
        check(api.process(h, input, 3, nullptr, &n) == 2 && n == 3, "caller buffer sizing");
        check(api.process(h, input, 3, output, &n) == 0 && output[2] == 6, "provider process");
        api.destroy(h);
        FreeLibrary(lib);
#endif
        std::cout << checks << " assertions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
