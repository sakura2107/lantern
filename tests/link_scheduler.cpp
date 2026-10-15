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
        std::cout << "{\"success\":true,\"checks\":5,\"aggregate_bytes\":524416,\"seconds\":" << elapsed
                  << ",\"input_wait_ms\":" << input_ms << ",\"cancel_ms\":" << cancel_ms << "}" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
