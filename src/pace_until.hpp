#pragma once
#include "lantern/win.hpp"
#include <thread>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
namespace lantern {

// An ordinary sleep wakes on the system timer tick (up to 15.6 ms late). At 120 fps the desktop
// loop overslept 4-16 ms (median 3.7 ms) and then sent the next frames back to back; two frames
// that land in one display refresh show as one on the phone. A high-resolution waitable timer
// (Windows 10 1803+) wakes within about half a millisecond; older systems fall back to sleep_until.
inline void pace_until(std::chrono::steady_clock::time_point target, bool short_wait = false) {
    struct Timer {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Timer() { if (h) CloseHandle(h); }
    };
    thread_local Timer timer;
    for (;;) {
        auto left = target - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero())
            return;
        LARGE_INTEGER due;
        due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
        if (!timer.h || !SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(timer.h, INFINITE) != WAIT_OBJECT_0) {
            if(short_wait){
                while(std::chrono::steady_clock::now()<target)std::this_thread::yield();
            }else std::this_thread::sleep_until(target);
            return;
        }
    }
}
} // namespace lantern
