// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>

#include "common/common_types.h"

/// Frame-level performance counters, to tell what a slow frame spent its time on (the D3D12
/// renderer logs them per hitch and per pacing window). Any thread adds; the presenter reads the
/// running totals and diffs them per frame, so nothing is ever reset.
namespace VideoCore::Perf {

enum class Counter : size_t {
    GpuThreadIdleUs,    ///< GPU thread waiting for guest command lists (the guest CPU is behind)
    GpuThreadFlushes,   ///< FlushRegion requests (the guest reading back GPU memory)
    GpuThreadFlushUs,
    Submits,            ///< command lists submitted to the queue
    FenceWaits,         ///< CPU waits on the GPU fence that actually blocked
    FenceWaitUs,
    GpuBusyUs,          ///< GPU time of the submitted command lists (timestamps)
    PipelineStalls,     ///< draws or dispatches that waited for a pipeline to be built
    PipelineStallUs,
    Draws,
    Dispatches,
    TextureUploads,
    TextureUploadBytes,
    TextureDownloads,
    TextureDownloadBytes,
    ResourcesCreated,   ///< committed resources created (images, staging, transfer buffers)
    ResourceCreateUs,
    StagingDedicated,      ///< staging buffers created outside the stream ring
    StagingDedicatedBytes,
    StagingStreamWaits,    ///< large uploads that waited for the GPU to free the stream ring
    TextureDecodeUs,       ///< CPU decoding/re-encoding of converted textures (ASTC, BCn arrays)
    TextureGpuDecodes,     ///< textures decoded by a compute shader (ASTC)
    DrawNs,                ///< whole draws (direct and indirect), nested counters included
    DrawTexturesNs,        ///< of which: reading texture handles and finding the image views
    DrawBuffersNs,         ///< uniform, storage, texel, vertex and index buffers
    DrawDescriptorsNs,     ///< writing the descriptor table (and each stage's host buffers)
    DrawTargetsNs,         ///< render targets, feedback loops and image transitions
    DrawSamplersNs,        ///< the sampler table
    DrawRecordNs,          ///< preparing attachments and recording state and the draw
    PipelineFastHits,      ///< transition-cache hits in CurrentGraphicsPipeline
    PipelineFastMisses,
    CbvCreateNs,
    CbvCreates,
    CbvStreamed,
    CbvPersistent,
    CbvNull,
    ViewCopyNs,
    ViewCopies,
    ClearNs,
    DispatchNs,
    GuestGpuWaits,         ///< guest fence waits (nvhost_ctrl events) that the GPU signalled later
    GuestGpuWaitUs,        ///< time from each of those waits to its signal; waits can overlap
    // The frame chain: game -> GPU thread -> composite -> vsync.
    Vsyncs,                ///< vsyncs processed by the VSyncThread (60 per second when on time)
    VsyncsLost,            ///< vsyncs skipped because the previous one ran late
    VsyncComposeWaitUs,    ///< the VSyncThread waiting for the GPU thread's previous composite
    VsyncFrames,           ///< vsyncs that composed a new game frame
    Composites,            ///< composites the renderer ran
    CompositeLatencyUs,    ///< from each composite request to the renderer running it
    CompositesLate,        ///< composites that ran more than a frame after the request
    GuestFramesQueued,     ///< frames the game queued (QueueBuffer)
    GuestDequeueWaits,     ///< times the game waited for a free framebuffer (DequeueBuffer)
    GuestDequeueWaitUs,
    GuestCoreIdleUs,       ///< emulated CPU cores with no guest thread to run, summed over cores
    Count,
};

constexpr size_t NUM_COUNTERS = static_cast<size_t>(Counter::Count);
using Snapshot = std::array<u64, NUM_COUNTERS>;

inline std::array<std::atomic<u64>, NUM_COUNTERS> counters{};
inline std::atomic<bool> detailed_gpu_profile{};

inline void SetDetailedGpuProfile(bool enabled) {
    detailed_gpu_profile.store(enabled, std::memory_order_relaxed);
}

[[nodiscard]] inline bool DetailedGpuProfileEnabled() {
    return detailed_gpu_profile.load(std::memory_order_relaxed);
}

inline void Add(Counter counter, u64 value) {
    counters[static_cast<size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

inline void AddDetailed(Counter counter, u64 value) {
    if (DetailedGpuProfileEnabled()) {
        Add(counter, value);
    }
}

inline Snapshot Read() {
    Snapshot snapshot{};
    for (size_t i = 0; i < NUM_COUNTERS; ++i) {
        snapshot[i] = counters[i].load(std::memory_order_relaxed);
    }
    return snapshot;
}

inline u64 Get(const Snapshot& snapshot, Counter counter) {
    return snapshot[static_cast<size_t>(counter)];
}

inline u64 ElapsedUs(std::chrono::steady_clock::time_point start) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

/// Adds the scope's duration to a microsecond counter (and 1 to count_counter, if given).
class ScopedTimer {
public:
    explicit ScopedTimer(Counter us_counter_, Counter count_counter_ = Counter::Count)
        : us_counter{us_counter_}, count_counter{count_counter_} {}
    ~ScopedTimer() {
        Add(us_counter, ElapsedUs(start));
        if (count_counter != Counter::Count) {
            Add(count_counter, 1);
        }
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Counter us_counter;
    Counter count_counter;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

/// Splits a function into consecutive phases: each Lap adds the nanoseconds since the previous one
/// (the phases of a draw last a few microseconds, too short to truncate to microseconds).
class LapTimer {
public:
    LapTimer() : enabled{DetailedGpuProfileEnabled()} {
        if (enabled) {
            last = std::chrono::steady_clock::now();
        }
    }

    void Lap(Counter ns_counter) {
        if (!enabled) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        Add(ns_counter, static_cast<u64>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last)
                                .count()));
        last = now;
    }

private:
    bool enabled{};
    std::chrono::steady_clock::time_point last{};
};

/// Adds the scope's duration to a nanosecond counter.
class ScopedNsTimer {
public:
    explicit ScopedNsTimer(Counter ns_counter_)
        : ns_counter{ns_counter_}, enabled{DetailedGpuProfileEnabled()} {
        if (enabled) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~ScopedNsTimer() {
        if (enabled) {
            Add(ns_counter,
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count()));
        }
    }

    ScopedNsTimer(const ScopedNsTimer&) = delete;
    ScopedNsTimer& operator=(const ScopedNsTimer&) = delete;

private:
    Counter ns_counter;
    bool enabled{};
    std::chrono::steady_clock::time_point start{};
};

} // namespace VideoCore::Perf
