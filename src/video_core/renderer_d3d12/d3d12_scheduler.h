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
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

/// Makes the direct list's calls on a worker thread for schedulers created from now on
/// (boot.cfg "async_record=1"), so the GPU thread only records them. Off: the GPU thread makes
/// every call itself, as the renderer did before the recorder.
void SetAsyncRecording(bool enabled);

/// Owns the direct command list and tracks GPU progress with monotonically increasing ticks, the
/// same model as the Vulkan backend's scheduler: CurrentTick() is the value the list being recorded
/// will signal when it is flushed, and every other object (staging memory, descriptor ranges,
/// released resources) is retired by comparing its tick with the GPU's.
///
/// Command allocators come from a pool and are reused once the tick they recorded is done: an
/// allocator must not be reset while the GPU still executes its commands.
///
/// Threading: only the recording thread (the GPU thread; whoever last asked for CommandList()) may
/// record or flush. Its calls reach the D3D12 list through a CommandStream, either at once or on
/// the recorder's worker, which also closes and submits each list (SubmitRecorded): Flush returns
/// once the submission is recorded, and SubmittedTick() says when it reached the queue. Other
/// threads - Eden's fence thread - may query and wait on ticks: if they wait on work that is not
/// flushed yet, they block until the recording thread flushes it instead of flushing a list that
/// thread is still writing to.
class Scheduler {
public:
    explicit Scheduler(Device& device);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// Records work for CurrentTick() into the direct list. Marks the caller as the recording
    /// thread.
    [[nodiscard]] CommandStream* CommandList() {
        recording_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        return &*stream;
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

    [[nodiscard]] bool HasGpuTimestamps() const noexcept {
        return timestamp_data != nullptr;
    }

    /// Submits the recorded work and waits for the GPU to finish it.
    void Finish();
    /// No submissions on a removed device; never throw from renderer/cache destructors.
    void DrainForShutdown() noexcept;

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

    /// IsFree without reading the fence: by the last tick seen completed (each submit and wait
    /// refreshes it). For per-draw checks that can fall back to IsFree when it says no.
    [[nodiscard]] bool IsKnownFree(u64 tick) const noexcept {
        return tick <= known_gpu_tick.load(std::memory_order_acquire);
    }

    /// The last tick whose list reached the queue. Work submitted to the same queue by another
    /// thread (the present thread's copy) must wait for it first: a queue runs lists in
    /// submission order, so submitting first would run before the work it depends on.
    [[nodiscard]] u64 SubmittedTick() const noexcept {
        return submitted_tick.load(std::memory_order_acquire);
    }
    /// Blocks until tick's list reached the queue; at once without a recording worker.
    void WaitSubmitted(u64 tick);

    /// Everything recorded up to now has reached the list once IsRecorded(RecordPoint()) holds:
    /// what the list reads on the CPU when it is called (the RTV/DSV behind a handle) may be
    /// reused from then on.
    [[nodiscard]] u64 RecordPoint() const noexcept {
        return recorder->RecordPoint();
    }
    [[nodiscard]] bool IsRecorded(u64 point) const noexcept {
        return recorder->IsExecuted(point);
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
    /// Resets the histogram. Only recorded with the detailed GPU profile: capturing a stack on
    /// every submission and wait costs CPU time on the recording thread.
    [[nodiscard]] std::string TakeSyncSites(size_t max_sites);

private:
    /// Counts the caller's stack in the sync histogram (recording thread only).
    void RecordSyncSite(char kind);
    struct PooledAllocator {
        ComPtr<ID3D12CommandAllocator> allocator;
        u64 tick;
    };

    ComPtr<ID3D12CommandAllocator> AcquireAllocator();

    /// Closes the list, submits it to signal tick and resets it for the next one. Recorded by
    /// Flush, so it runs where the list is recorded (the worker or the recording thread).
    void SubmitRecorded(ID3D12GraphicsCommandList* list, u64 tick);
    /// On the worker after a recorded call threw: releases every waiter, so the recording thread
    /// reaches its next Flush or Wait, which rethrows the error.
    void ReleaseWaitersAfterError() noexcept;

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

    // Owned by whoever records the list's calls: the worker, or the recording thread without one.
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12CommandAllocator> current_allocator;
    std::deque<PooledAllocator> allocator_pool;

    /// Records the calls on command_list (see CommandStream).
    std::optional<ListRecorder> recorder;
    std::optional<CommandStream> stream;
    ID3D12PipelineState* current_pipeline{};

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
    std::atomic<u64> submitted_tick{0};
    mutable std::atomic<u64> known_gpu_tick{0};
};

} // namespace D3D12
