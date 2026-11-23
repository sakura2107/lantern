#include "lantern/link_scheduler.hpp"
#include <iostream>
#include <thread>
using namespace lantern;
int main() {
    try {
        LinkScheduler scheduler(256 * 1024);
        std::atomic_bool stop = false;
        std::atomic_uint bulk = 0;
        std::atomic_bool begun = false;
        auto start = std::chrono::steady_clock::now();
        std::thread file([&] {
            for (unsigned i = 0; i < 32; ++i) {
                scheduler.acquire(1, Lane::bulk, 16384, [&] { return stop.load(); });
                ++bulk;
                begun = true;
            }
            scheduler.forget(1);
        });
        while (!begun)
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        auto input_start = std::chrono::steady_clock::now();
        scheduler.acquire(2, Lane::input, 128, [] { return false; });
        auto input_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - input_start).count();
        file.join();
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (bulk != 32 || elapsed < 1.7 || elapsed > 5 || input_ms > 300)
            throw std::runtime_error("aggregate pacing, bulk progress or control delay");
        // Cancellation while waiting on an exhausted bucket must not wait for a record's allowance.
        scheduler.rate(65536);
        std::atomic_bool cancelled = false;
        bool caught = false;
        std::thread waiter([&] {
            try {
                for (int i = 0; i < 20; i++)
                    scheduler.acquire(3, Lane::bulk, 16384, [&] { return cancelled.load(); });
            } catch (const Error& e) {
                caught = e.code == Code::cancelled;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto cancel_start = std::chrono::steady_clock::now();
        cancelled = true;
        waiter.join();
        auto cancel_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancel_start)
                .count();
        if (!caught || cancel_ms > 100)
            throw std::runtime_error("scheduler cancellation");
        bool invalid = false;
        try {
            scheduler.acquire(4, Lane::bulk, 32769, [] { return false; });
        } catch (const Error& e) {
            invalid = e.code == Code::invalid;
        }
        if (!invalid)
            throw std::runtime_error("record bound");

        // Exclusive bulk flow: uncontended bulk uses the full aggregate rate (no fixed 25% cap).
        constexpr double exclusive_rate = 512.0 * 1024.0;
        LinkScheduler exclusive(exclusive_rate);
        constexpr unsigned exclusive_records = 32;
        constexpr size_t record_bytes = 16384;
        auto ex_start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < exclusive_records; ++i)
            exclusive.acquire(10, Lane::bulk, record_bytes, [] { return false; });
        exclusive.forget(10);
        auto exclusive_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - ex_start).count();
        const double exclusive_bytes = double(exclusive_records) * double(record_bytes);
        if (exclusive_s < 0.75 || exclusive_s > 2.2)
            throw std::runtime_error("exclusive bulk flow should saturate full aggregate rate");

        // Mixed concurrent run: desktop (weight 8) + bulk (weight 1) + periodic input (weight 32).
        constexpr double mixed_rate = 512.0 * 1024.0;
        LinkScheduler mixed(mixed_rate);
        std::atomic_bool mixed_stop = false;
        std::atomic<uint64_t> desktop_bytes = 0, bulk_bytes = 0, input_bytes = 0;
        auto mix_start = std::chrono::steady_clock::now();
        std::thread desktop_worker([&] {
            try {
                while (!mixed_stop.load()) {
                    mixed.acquire(20, Lane::desktop, record_bytes, [&] { return mixed_stop.load(); });
                    desktop_bytes += record_bytes;
                }
            } catch (const Error& e) {
                if (e.code != Code::cancelled)
                    throw;
            }
            mixed.forget(20);
        });
        std::thread bulk_worker([&] {
            try {
                while (!mixed_stop.load()) {
                    mixed.acquire(21, Lane::bulk, record_bytes, [&] { return mixed_stop.load(); });
                    bulk_bytes += record_bytes;
                }
            } catch (const Error& e) {
                if (e.code != Code::cancelled)
                    throw;
            }
            mixed.forget(21);
        });
        double max_mixed_input_ms = 0;
        for (int probe = 0; probe < 6; ++probe) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto pstart = std::chrono::steady_clock::now();
            mixed.acquire(22, Lane::input, 128, [] { return false; });
            auto pms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pstart).count();
            max_mixed_input_ms = std::max(max_mixed_input_ms, pms);
            input_bytes += 128;
        }
        mixed.forget(22);
        mixed_stop = true;
        desktop_worker.join();
        bulk_worker.join();
        auto mixed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - mix_start).count();
        const uint64_t total_mixed = desktop_bytes.load() + bulk_bytes.load() + input_bytes.load();
        const double max_allowed = mixed_rate * mixed_s + 32768.0 + double(record_bytes) * 2.0;
        if (desktop_bytes.load() < bulk_bytes.load() * 3 || bulk_bytes.load() == 0 ||
            double(total_mixed) > max_allowed || max_mixed_input_ms > 200.0)
            throw std::runtime_error("mixed weighted fairness, aggregate bound or input priority failed");

        std::cout << "{\"success\":true,\"checks\":9,\"aggregate_bytes\":524416,\"seconds\":" << elapsed
                  << ",\"input_wait_ms\":" << input_ms << ",\"cancel_ms\":" << cancel_ms
                  << ",\"exclusive_bytes\":" << uint64_t(exclusive_bytes)
                  << ",\"exclusive_seconds\":" << exclusive_s
                  << ",\"mixed_seconds\":" << mixed_s
                  << ",\"mixed_desktop_bytes\":" << desktop_bytes.load()
                  << ",\"mixed_bulk_bytes\":" << bulk_bytes.load()
                  << ",\"mixed_input_max_ms\":" << max_mixed_input_ms << "}" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
