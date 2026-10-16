// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include "video_core/perf_counters.h"

namespace D3D12::DrawMetrics {

// Session-wide distributions, separate from the presenter's delta counters. Opt-in only;
// normal builds do not read clocks, update bins or count IA views. No per-draw allocations.
inline std::array<std::atomic<u64>, 64> draw_bins{};
inline std::atomic<u64> max_draw_ns{};
inline std::atomic<u64> sampler_resolve_requests{}, sampler_resolve_reused{};
inline std::atomic<u64> uniform_upload_bytes{};
// <=64, <=128, <=256, <=512, >512 bytes. These count actual streamed bindings.
inline std::array<std::atomic<u64>, 5> uniform_upload_sizes{};
inline std::atomic<u64> stage_binding_samples{}, stage_binding_sample_ns{};

// Sample all active stages of one draw in 64, avoiding aliasing to one shader stage.
// This is nested elapsed time, not CPU busy time or a separate additive draw phase.
inline bool SampleStageBindings() {
    if (!VideoCore::Perf::DetailedGpuProfileEnabled()) return false;
    static thread_local u32 sequence{};
    return (++sequence & 63U) == 0;
}
class SampledStageBindingTimer {
public:
    explicit SampledStageBindingTimer(bool sample) : enabled{sample} {
        if (enabled) start = std::chrono::steady_clock::now();
    }
    ~SampledStageBindingTimer() {
        if (!enabled) return;
        stage_binding_sample_ns.fetch_add(static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        stage_binding_samples.fetch_add(1, std::memory_order_relaxed);
    }
private:
    bool enabled{};
    std::chrono::steady_clock::time_point start{};
};

constexpr size_t Bin(u64 ns) noexcept {
    return (std::min)(static_cast<size_t>(std::bit_width(ns)), size_t{63});
}

inline std::atomic<u64> cache_lock_ns{}, cache_lock_samples{}, cache_lock_long{}, cache_lock_max_ns{};

// Measure acquisition, not protected work. The caller retains the original std::scoped_lock.
// Elapsed samples can include descheduling as well as contention.
class CacheLockTimer {
public:
    CacheLockTimer() : enabled{VideoCore::Perf::DetailedGpuProfileEnabled()} {
        if (enabled) start = std::chrono::steady_clock::now();
    }
    void Locked() const {
        if (!enabled) return;
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        cache_lock_ns.fetch_add(ns, std::memory_order_relaxed);
        cache_lock_samples.fetch_add(1, std::memory_order_relaxed);
        if (ns >= 200'000) cache_lock_long.fetch_add(1, std::memory_order_relaxed);
        u64 previous = cache_lock_max_ns.load(std::memory_order_relaxed);
        while (previous < ns && !cache_lock_max_ns.compare_exchange_weak(
                   previous, ns, std::memory_order_relaxed)) {}
    }
private:
    bool enabled{};
    std::chrono::steady_clock::time_point start{};
};

class ScopedDrawTimer {
public:
    ScopedDrawTimer() : enabled{VideoCore::Perf::DetailedGpuProfileEnabled()} {
        if (enabled) start = std::chrono::steady_clock::now();
    }
    ~ScopedDrawTimer() {
        if (!enabled) return;
        const auto ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        VideoCore::Perf::Add(VideoCore::Perf::Counter::DrawNs, ns);
        draw_bins[Bin(ns)].fetch_add(1, std::memory_order_relaxed);
        u64 previous = max_draw_ns.load(std::memory_order_relaxed);
        while (previous < ns && !max_draw_ns.compare_exchange_weak(
                   previous, ns, std::memory_order_relaxed)) {}
    }
    ScopedDrawTimer(const ScopedDrawTimer&) = delete;
    ScopedDrawTimer& operator=(const ScopedDrawTimer&) = delete;

private:
    bool enabled{};
    std::chrono::steady_clock::time_point start{};
};

} // namespace D3D12::DrawMetrics
