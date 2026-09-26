// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <functional>
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
class Scheduler {
public:
    explicit Scheduler(Device& device);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// The list recording work for CurrentTick().
    [[nodiscard]] ID3D12GraphicsCommandList* CommandList() const {
        return command_list.Get();
    }

    /// Submits the recorded work; returns the tick it signals.
    u64 Flush();

    /// Submits the recorded work and waits for the GPU to finish it.
    void Finish();

    /// Waits until the GPU has passed tick, flushing first if tick is still being recorded.
    void Wait(u64 tick);

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick;
    }

    /// Last tick the GPU is known to have completed (refreshed from the fence).
    [[nodiscard]] u64 KnownGpuTick() const;

    [[nodiscard]] bool IsFree(u64 tick) const {
        return tick <= known_gpu_tick || tick <= KnownGpuTick();
    }

    /// Keeps object alive until the GPU is done with everything recorded so far.
    void DeferRelease(ComPtr<IUnknown> object);

    /// Called right before each submission (e.g. to end open queries or flush barriers).
    void RegisterOnSubmit(std::function<void()>&& func) {
        on_submit = std::move(func);
    }

    /// Frees retired allocators and released objects; called on every flush and frame.
    void CollectGarbage();

private:
    struct PooledAllocator {
        ComPtr<ID3D12CommandAllocator> allocator;
        u64 tick;
    };

    ComPtr<ID3D12CommandAllocator> AcquireAllocator();

    Device& device;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};

    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12CommandAllocator> current_allocator;
    std::deque<PooledAllocator> allocator_pool;

    std::deque<std::pair<u64, ComPtr<IUnknown>>> pending_releases;
    std::function<void()> on_submit;

    u64 current_tick{1};
    mutable u64 known_gpu_tick{0};
};

} // namespace D3D12
