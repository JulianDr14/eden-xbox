// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <unordered_map>

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
    TextureUploadMapNs,    ///< querying a CPU pointer for non-staging upload sources
    TextureUploadRepackNs, ///< CPU row repacking into a D3D12-aligned footprint
    TextureUploadRecordNs, ///< CopyTextureRegion recording
    TextureUploadMaps,
    TextureUploadRepacks,
    TextureUploadCopies,
    TextureCacheFindNs,
    TextureCacheFinds,
    TextureCacheInsertNs,
    TextureCacheInserts,
    TextureCacheOverlapNs,
    TextureCacheImageCreateNs,
    TextureCacheRefreshNs,
    TextureCacheStagingNs,
    TextureCacheUnswizzleNs,
    TextureCacheBackendUploadNs,
    TextureCacheRegisterNs,
    TextureCacheViewCreateNs,
    TextureCacheViewsCreated,
    TextureCacheViewSetupNs,       ///< format, swizzle, range and dimension derivation
    TextureCacheViewReinterpretNs, ///< typeless-family validation/reinterpreted image lookup
    TextureCacheViewSrvNs,         ///< SRV parameters and eager natural SRV creation
    TextureCacheViewAttachmentNs,  ///< RTV/DSV/UAV capability checks and descriptor creation
    TextureCacheViewSlotInsertNs,  ///< SlotVector insertion when it already has a free slot
    TextureCacheViewSlotGrowNs,    ///< SlotVector allocation and relocation plus inserted view
    TextureCacheViewSlotGrows,
    TextureCacheViewSlotClockNs,  ///< back-to-back steady_clock calls in profiled insertion
    TextureCacheViewSlotFreeNs,   ///< selecting/growing the backing slot
    TextureCacheViewSlotConstructNs, ///< placement-new, including member initializers
    TextureCacheViewSlotBitNs,    ///< marking the selected slot occupied
    TextureCacheViewIndexNs,       ///< linking the new view into its owning Image
    TextureCacheViewBaseNs,        ///< ImageViewBase constructor body
    TextureCacheViewCompatibilityNs, ///< IsViewCompatible inside ImageViewBase
    DrawNs,                ///< whole draws (direct and indirect), nested counters included
    DrawTexturesNs,        ///< of which: reading texture handles and finding the image views
    DrawBuffersNs,         ///< uniform, storage, texel, vertex and index buffers
    DrawDescriptorsNs,     ///< writing the descriptor table (and each stage's host buffers)
    DrawTargetsNs,         ///< render targets, feedback loops and image transitions
    DrawUpdateTargetsNs,
    DrawFeedbackNs,
    DrawFramebufferNs,
    DrawImageTransitionsNs,
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
    SubmitListNs,
    SubmitLists,
    GpuTickNs,
    GpuTicks,
    CacheInvalidationNs,
    CacheInvalidations,
    DmaPullerNs,
    DmaPullerCalls,
    DmaMacroNs,
    DmaMacroCalls,
    DmaMaxwellNs,
    DmaMaxwellCalls,
    DmaComputeNs,
    DmaComputeCalls,
    DmaCopyNs,
    DmaCopyCalls,
    DmaOtherNs,
    DmaOtherCalls,
    MaxwellDirtyChanged,
    MaxwellDirtyUnchanged,
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
    GuestSwapIntervalOverrides, ///< diagnostic requests above one forced back to one
    GuestDequeueWaits,     ///< times the game waited for a free framebuffer (DequeueBuffer)
    GuestDequeueWaitUs,
    GuestCoreIdleUs,       ///< emulated CPU cores with no guest thread to run, summed over cores
    GuestCoreWakesSpun,    ///< idle cores interrupted while spinning (no OS wake)
    GuestCoreWakesBlocked, ///< idle cores interrupted after blocking in the OS
    Count,
};

constexpr size_t NUM_COUNTERS = static_cast<size_t>(Counter::Count);
using Snapshot = std::array<u64, NUM_COUNTERS>;

inline std::array<std::atomic<u64>, NUM_COUNTERS> counters{};
inline std::atomic<bool> detailed_gpu_profile{};
inline std::atomic<bool> force_swap_interval_one{};

constexpr size_t NUM_GUEST_SYNCPOINTS = 256;
struct GuestWaitSiteCounters {
    std::atomic<u64> count{};
    std::atomic<u64> total_us{};
    std::atomic<u64> max_us{};
};
struct GuestWaitSiteSnapshot {
    u64 count{};
    u64 total_us{};
    u64 max_us{};
};
inline std::array<GuestWaitSiteCounters, NUM_GUEST_SYNCPOINTS> guest_wait_sites{};

struct MacroProfileSnapshot {
    u32 method{};
    u64 hash{};
    u64 count{};
    u64 total_ns{};
    u64 nested_ns{};
    u64 max_ns{};
};
inline std::mutex macro_profiles_mutex;
inline std::unordered_map<u32, MacroProfileSnapshot> macro_profiles;

inline void SetDetailedGpuProfile(bool enabled) {
    detailed_gpu_profile.store(enabled, std::memory_order_relaxed);
}

[[nodiscard]] inline bool DetailedGpuProfileEnabled() {
    return detailed_gpu_profile.load(std::memory_order_relaxed);
}

inline void SetForceSwapIntervalOne(bool enabled) {
    force_swap_interval_one.store(enabled, std::memory_order_relaxed);
}

[[nodiscard]] inline bool ForceSwapIntervalOne() {
    return force_swap_interval_one.load(std::memory_order_relaxed);
}

inline void Add(Counter counter, u64 value) {
    counters[static_cast<size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

inline void AddDetailed(Counter counter, u64 value) {
    if (DetailedGpuProfileEnabled()) {
        Add(counter, value);
    }
}

[[nodiscard]] inline u64 ReadCounter(Counter counter) {
    return counters[static_cast<size_t>(counter)].load(std::memory_order_relaxed);
}

inline void RecordMacroProfile(u32 method, u64 hash, u64 total_ns, u64 nested_ns) {
    if (!DetailedGpuProfileEnabled()) {
        return;
    }
    Add(Counter::DmaMacroCalls, 1);
    Add(Counter::DmaMacroNs, total_ns);
    std::scoped_lock lock{macro_profiles_mutex};
    auto& profile = macro_profiles[method];
    profile.method = method;
    profile.hash = hash;
    ++profile.count;
    profile.total_ns += total_ns;
    profile.nested_ns += nested_ns;
    profile.max_ns = std::max(profile.max_ns, total_ns);
}

inline std::vector<MacroProfileSnapshot> TakeMacroProfiles() {
    std::scoped_lock lock{macro_profiles_mutex};
    std::vector<MacroProfileSnapshot> result;
    result.reserve(macro_profiles.size());
    for (const auto& [method, profile] : macro_profiles) {
        result.push_back(profile);
    }
    macro_profiles.clear();
    return result;
}

inline void RecordGuestGpuWait(u32 syncpoint_id, u64 waited_us) {
    Add(Counter::GuestGpuWaits, 1);
    Add(Counter::GuestGpuWaitUs, waited_us);
    if (syncpoint_id >= guest_wait_sites.size()) {
        return;
    }
    auto& site = guest_wait_sites[syncpoint_id];
    site.count.fetch_add(1, std::memory_order_relaxed);
    site.total_us.fetch_add(waited_us, std::memory_order_relaxed);
    u64 previous = site.max_us.load(std::memory_order_relaxed);
    while (previous < waited_us &&
           !site.max_us.compare_exchange_weak(previous, waited_us, std::memory_order_relaxed)) {
    }
}

inline std::array<GuestWaitSiteSnapshot, NUM_GUEST_SYNCPOINTS> TakeGuestWaitSites() {
    std::array<GuestWaitSiteSnapshot, NUM_GUEST_SYNCPOINTS> result{};
    for (size_t index = 0; index < result.size(); ++index) {
        auto& site = guest_wait_sites[index];
        result[index] = {.count = site.count.exchange(0, std::memory_order_relaxed),
                         .total_us = site.total_us.exchange(0, std::memory_order_relaxed),
                         .max_us = site.max_us.exchange(0, std::memory_order_relaxed)};
    }
    return result;
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
