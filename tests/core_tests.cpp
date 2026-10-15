#include "lantern/av_timeline.hpp"
#include "lantern/core.hpp"
#include "lantern/plugin.h"
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
