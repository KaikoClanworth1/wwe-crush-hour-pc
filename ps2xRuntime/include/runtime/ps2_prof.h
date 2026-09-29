#pragma once

// Tiny always-on profiling counters (nanoseconds + call counts), printed by the PS2X_STATUS line.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ps2prof
{
    enum Counter : int
    {
        kVu1 = 0,
        kGsSubmit,
        kGsFlush,
        kGsPresent,
        kMpegDecode,
        kMpegFrameWrite,
        kGsLockWait,
        kHostPresent,
        kSchedIdle,
        kSchedPace,
        kCount
    };

    inline std::atomic<uint64_t> g_ns[kCount]{};
    inline std::atomic<uint64_t> g_calls[kCount]{};

    inline const char *name(int c)
    {
        static const char *names[kCount] = {"vu1", "gsSubmit", "gsFlush", "gsPresent", "mpegDecode", "mpegWrite", "gsLockWait", "hostPresent", "schedIdle", "schedPace"};
        return names[c];
    }

    struct Scope
    {
        explicit Scope(Counter c) : counter(c), start(std::chrono::steady_clock::now()) {}
        ~Scope()
        {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            g_ns[counter].fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
            g_calls[counter].fetch_add(1u, std::memory_order_relaxed);
        }
        Counter counter;
        std::chrono::steady_clock::time_point start;
    };
}
