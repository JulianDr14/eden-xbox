// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

/// Owns the direct command list and tracks GPU progress with monotonically increasing ticks, the
/// same model as the Vulkan backend's scheduler: CurrentTick() is the value the list being recorded
/// will signal when it is flushed, and every other object (staging memory, descriptor ranges,
/// released resources) is retired by comparing its tick with the GPU's.
///
/// Command allocators come from a pool and are reused once the tick they recorded is done: an
/// allocator must not be reset while the GPU still executes its commands.
///
/// Threading: commands are recorded straight into one D3D12 list, which is not thread-safe, so only
/// the recording thread (the GPU thread; whoever last asked for CommandList()) may record or flush.
/// Other threads - Eden's fence thread - may query and wait on ticks: if they wait on work that is
/// not submitted yet, they block until the recording thread submits it instead of flushing a list
/// that thread is still writing to.
class Scheduler {
public:
    explicit Scheduler(Device& device);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// The list recording work for CurrentTick(). Marks the caller as the recording thread.
    [[nodiscard]] ID3D12GraphicsCommandList* CommandList() {
        recording_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        return command_list.Get();
    }

    /// Graphics and compute share one PSO binding on a direct list. All users, including internal
    /// texture conversions, must go through this cache so the next draw restores its graphics PSO.
    void SetPipelineState(ID3D12PipelineState* pipeline) {
        if (current_pipeline != pipeline) {
            CommandList()->SetPipelineState(pipeline);
            current_pipeline = pipeline;
        }
    }

    /// Submits the recorded work; returns the tick it signals.
    u64 Flush();

    /// Submits the recorded work and waits for the GPU to finish it.
    void Finish();

    /// Waits until the GPU has passed tick. On the recording thread, flushes first if tick is still
    /// being recorded; on any other thread, waits for the recording thread to submit it.
    void Wait(u64 tick);

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    /// Last tick the GPU is known to have completed (refreshed from the fence).
    [[nodiscard]] u64 KnownGpuTick() const;

    [[nodiscard]] bool IsFree(u64 tick) const {
        return tick <= known_gpu_tick.load(std::memory_order_acquire) || tick <= KnownGpuTick();
    }

    /// Whether the caller is the thread recording into CommandList().
    [[nodiscard]] bool IsRecordingThread() const {
        return recording_thread.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

    /// Keeps object alive until the GPU is done with everything recorded so far.
    void DeferRelease(ComPtr<IUnknown> object);
    /// Releases object and then runs retire after the same GPU tick completes. Used by placed
    /// resources to return their heap range only after the resource itself is no longer in use.
    void DeferRelease(ComPtr<IUnknown> object, std::function<void()>&& retire);

    /// Called right before each submission (e.g. to end open queries or flush barriers).
    void RegisterOnSubmit(std::function<void()>&& func) {
        on_submit.emplace_back(std::move(func));
    }

    /// Called after the command list has been reset for a new submission.
    void RegisterOnReset(std::function<void()>&& func) {
        on_reset.emplace_back(std::move(func));
    }

    void ClearSubmissionCallbacks() {
        on_submit.clear();
        on_reset.clear();
    }

    /// Frees retired allocators and released objects; called on every flush and frame.
    void CollectGarbage();

    /// The most frequent call stacks of submissions and blocking waits on the recording thread
    /// since the last call, as "count x rva<rva<..." (eden-uwp.exe RVAs for the build's PDB).
    /// Resets the histogram.
    [[nodiscard]] std::string TakeSyncSites(size_t max_sites);

private:
    /// Counts the caller's stack in the sync histogram (recording thread only).
    void RecordSyncSite(char kind);
    struct PooledAllocator {
        ComPtr<ID3D12CommandAllocator> allocator;
        u64 tick;
    };

    ComPtr<ID3D12CommandAllocator> AcquireAllocator();

    /// GPU time of every submission, from timestamps at the start and end of its list
    /// (VideoCore::Perf GpuBusyUs). Begin runs on a fresh list, End right before it closes.
    void CreateTimestamps();
    void BeginTimestamp();
    void EndTimestamp(u64 tick);
    /// Adds the timings of the submissions the GPU has finished.
    void ReadTimestamps(u64 gpu_tick);

    static constexpr u32 TIMESTAMP_SLOTS = 64;
    ComPtr<ID3D12QueryHeap> timestamp_heap;
    ComPtr<ID3D12Resource> timestamp_readback;
    const u64* timestamp_data{};
    double timestamp_us_per_tick{};
    u32 timestamp_next{};
    u32 timestamp_slot{};
    bool timestamp_open{};
    std::mutex timestamp_mutex;
    std::deque<std::pair<u64, u32>> pending_timestamps; ///< (tick, slot)

    static constexpr size_t SYNC_SITE_FRAMES = 6;
    /// Kind ('S' submit, 'W' wait) and return addresses -> count.
    std::mutex sync_sites_mutex;
    std::map<std::pair<char, std::array<uintptr_t, SYNC_SITE_FRAMES>>, u64> sync_sites;

    Device& device;
    ComPtr<ID3D12Fence> fence;

    ComPtr<ID3D12GraphicsCommandList> command_list;
    ID3D12PipelineState* current_pipeline{};
    ComPtr<ID3D12CommandAllocator> current_allocator;
    std::deque<PooledAllocator> allocator_pool;

    std::mutex release_mutex;
    struct PendingRelease {
        u64 tick;
        ComPtr<IUnknown> object;
        std::function<void()> retire;
    };
    std::deque<PendingRelease> pending_releases;
    std::vector<std::function<void()>> on_submit;
    std::vector<std::function<void()>> on_reset;

    /// Serializes submissions; recursive because submit callbacks may end queries.
    std::recursive_mutex submit_mutex;
    std::mutex submitted_mutex;
    std::condition_variable submitted_cv;

    std::atomic<std::thread::id> recording_thread{std::this_thread::get_id()};
    std::atomic<u64> current_tick{1};
    mutable std::atomic<u64> known_gpu_tick{0};
};

} // namespace D3D12
