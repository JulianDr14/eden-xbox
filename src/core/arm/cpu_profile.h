// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>

#include "common/common_types.h"

namespace Core::CpuProfile {

enum class Counter : size_t { Runs, RunNs, CodeWords, Reads, Reads128, Writes, ClockReads, Count };
using Snapshot = std::array<u64, static_cast<size_t>(Counter::Count)>;
struct alignas(64) CoreCounters {
    std::array<std::atomic<u64>, static_cast<size_t>(Counter::Count)> values{};
};
inline std::array<CoreCounters, 4> cores;
inline std::atomic_bool enabled{};

inline void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }
[[nodiscard]] inline bool Enabled() { return enabled.load(std::memory_order_relaxed); }
inline void Add(size_t core, Counter counter, u64 value = 1) {
    if (Enabled()) {
        cores.at(core).values[static_cast<size_t>(counter)].fetch_add(value,
                                                                   std::memory_order_relaxed);
    }
}
[[nodiscard]] inline Snapshot Take(size_t core) {
    Snapshot result{};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = cores.at(core).values[i].exchange(0, std::memory_order_relaxed);
    }
    return result;
}
[[nodiscard]] inline u64 Get(const Snapshot& snapshot, Counter counter) {
    return snapshot[static_cast<size_t>(counter)];
}

/// Time inside Run(), including JIT translation, callbacks and host preemption. It is elapsed
/// time, not host CPU utilization. Disabled during normal play: no clock reads or atomic adds.
class RunTimer {
public:
    explicit RunTimer(size_t core_) : core{core_}, active{Enabled()} {
        if (active) start = std::chrono::steady_clock::now();
    }
    ~RunTimer() {
        if (!active) return;
        const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start).count();
        Add(core, Counter::RunNs, ns);
        Add(core, Counter::Runs);
    }
private:
    size_t core;
    bool active;
    std::chrono::steady_clock::time_point start;
};

} // namespace Core::CpuProfile
