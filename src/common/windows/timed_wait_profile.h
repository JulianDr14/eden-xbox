// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include "common/common_types.h"
namespace Common::Windows::TimedWaitProfile {
// Separate opt-in from GPU profiling. Only HostTiming activates its thread; other Event users
// remain unmeasured. Elapsed spin is not CPU busy time (MWAITX can suspend execution).
inline std::atomic_bool enabled{};
inline thread_local bool current_thread{};
struct Statistics {
    u64 waits{}, requested_ns{}, elapsed_ns{}, spin_ns{}, sleeps{}, sleep_ns{};
    u64 fallbacks{}, margin_max_ns{}, late_max_ns{};
    std::array<u64, 4> durations{}; // <=100us, <=250us, <=1ms, >1ms
};
inline std::array<std::atomic<u64>, 13> counters{};
inline void ActivateThread() { current_thread = enabled.load(std::memory_order_relaxed); }
inline void Max(size_t i, u64 n) {
    u64 previous = counters[i].load(std::memory_order_relaxed);
    while (previous < n && !counters[i].compare_exchange_weak(previous, n, std::memory_order_relaxed)) {}
}
inline Statistics Read() {
    Statistics s{counters[0].load(), counters[1].load(), counters[2].load(), counters[3].load(),
                 counters[4].load(), counters[5].load(), counters[6].load(), counters[7].load(), counters[8].load()};
    for(size_t i=0;i<4;++i)s.durations[i]=counters[9+i].load();
    return s;
}
class Sample {
public:
    explicit Sample(s64 requested) : active{current_thread} {
        if (!active) return;
        start = Clock::now();
        counters[0].fetch_add(1, std::memory_order_relaxed);
        const u64 n = requested > 0 ? static_cast<u64>(requested) : 0;
        counters[1].fetch_add(n, std::memory_order_relaxed);
        const size_t bucket = n <= 100000 ? 0 : n <= 250000 ? 1 : n <= 1000000 ? 2 : 3;
        counters[9+bucket].fetch_add(1, std::memory_order_relaxed);
        requested_ns = n;
    }
    void Margin(s64 n) const { if(active && n>0) Max(7,static_cast<u64>(n)); }
    void Fallback() const { if(active)counters[6].fetch_add(1,std::memory_order_relaxed); }
    void BeginSleep() { if(active)sleep_start=Clock::now(); }
    void EndSleep() const {
        if(!active)return;
        counters[4].fetch_add(1,std::memory_order_relaxed);
        counters[5].fetch_add(Ns(Clock::now()-sleep_start),std::memory_order_relaxed);
    }
    void BeginSpin() { if(active){spinning=true;spin_start=Clock::now();} }
    ~Sample() {
        if(!active)return;
        const auto end=Clock::now();
        const u64 elapsed=Ns(end-start);
        counters[2].fetch_add(elapsed,std::memory_order_relaxed);
        if(spinning)counters[3].fetch_add(Ns(end-spin_start),std::memory_order_relaxed);
        if(elapsed>requested_ns)Max(8,elapsed-requested_ns);
    }
private:
    using Clock=std::chrono::steady_clock;
    static u64 Ns(Clock::duration d){return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());}
    bool active{},spinning{};
    u64 requested_ns{};
    Clock::time_point start{},sleep_start{},spin_start{};
};
} // namespace Common::Windows::TimedWaitProfile
