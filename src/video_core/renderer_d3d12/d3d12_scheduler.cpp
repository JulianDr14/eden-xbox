// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <exception>
#include <stdexcept>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

namespace {

/// One auto-reset event per waiting thread. A shared event is unsafe: the GPU thread and the fence
/// thread may wait at the same time, and one would consume the other's wake-up.
HANDLE ThreadWaitEvent() {
    struct Event {
        HANDLE handle = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        ~Event() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    thread_local Event event;
    if (event.handle == nullptr) {
        throw std::runtime_error("D3D12: CreateEventExW failed");
    }
    return event.handle;
}

void StoreMax(std::atomic<u64>& target, u64 value) {
    u64 current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_release,
                                         std::memory_order_relaxed)) {
    }
}

} // Anonymous namespace

Scheduler::Scheduler(Device& device_) : device{device_} {
    ID3D12Device* const dev = device.Get();
    ThrowIfFailed(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                  "CreateFence (scheduler)");
    current_allocator = AcquireAllocator();
    ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         current_allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&command_list)),
                  "CreateCommandList");
}

Scheduler::~Scheduler() {
    try {
        // Teardown runs on whichever thread destroys the renderer; nothing records anymore.
        recording_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        Finish();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "{}", e.what());
    }
    std::scoped_lock lock{release_mutex};
    pending_releases.clear();
}

u64 Scheduler::Flush() {
    std::scoped_lock lock{submit_mutex};
    for (auto& callback : on_submit) {
        callback();
    }
    ThrowIfFailed(command_list->Close(), "ID3D12GraphicsCommandList::Close");
    ID3D12CommandList* const lists[] = {command_list.Get()};
    device.Queue()->ExecuteCommandLists(1, lists);
    CheckRemovedAfter(device.Get(), [&] {
        return fmt::format("submitting tick {} (removed by the GPU or by a recorded command)",
                           current_tick.load(std::memory_order_relaxed));
    });

    const u64 signaled = current_tick.load(std::memory_order_relaxed);
    ThrowIfFailed(device.Queue()->Signal(fence.Get(), signaled), "ID3D12CommandQueue::Signal");
    allocator_pool.push_back({std::move(current_allocator), signaled});
    {
        // Publish the submission to threads waiting on it (see Wait).
        std::scoped_lock submitted_lock{submitted_mutex};
        current_tick.store(signaled + 1, std::memory_order_release);
    }
    submitted_cv.notify_all();

    CollectGarbage();
    current_allocator = AcquireAllocator();
    ThrowIfFailed(command_list->Reset(current_allocator.Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset");
    for (auto& callback : on_reset) {
        callback();
    }
    return signaled;
}

void Scheduler::Finish() {
    Wait(Flush());
    CollectGarbage();
}

void Scheduler::Wait(u64 tick) {
    if (tick >= CurrentTick()) {
        if (IsRecordingThread()) {
            Flush();
        } else {
            // Never flush a list another thread is recording into: wait for it to be submitted.
            std::unique_lock lock{submitted_mutex};
            submitted_cv.wait(lock, [this, tick] { return tick < CurrentTick(); });
        }
    }
    if (IsFree(tick)) {
        return;
    }
    const HANDLE event = ThreadWaitEvent();
    ThrowIfFailed(fence->SetEventOnCompletion(tick, event), "ID3D12Fence::SetEventOnCompletion");
    WaitForSingleObjectEx(event, INFINITE, FALSE);
    StoreMax(known_gpu_tick, tick);
}

u64 Scheduler::KnownGpuTick() const {
    const u64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        // GetCompletedValue returns all ones once the device is removed.
        device.ReportDeviceRemoved();
        throw std::runtime_error(fmt::format("D3D12: device removed (reason 0x{:08X})",
                                             static_cast<u32>(
                                                 device.Get()->GetDeviceRemovedReason())));
    }
    StoreMax(known_gpu_tick, completed);
    return known_gpu_tick.load(std::memory_order_acquire);
}

void Scheduler::DeferRelease(ComPtr<IUnknown> object) {
    if (object) {
        std::scoped_lock lock{release_mutex};
        pending_releases.emplace_back(CurrentTick(), std::move(object));
    }
}

void Scheduler::CollectGarbage() {
    const u64 gpu_tick = KnownGpuTick();
    std::scoped_lock lock{release_mutex};
    while (!pending_releases.empty() && pending_releases.front().first <= gpu_tick) {
        pending_releases.pop_front();
    }
}

ComPtr<ID3D12CommandAllocator> Scheduler::AcquireAllocator() {
    // Submission order keeps the pool sorted by tick, so only the front can be free.
    if (!allocator_pool.empty() && IsFree(allocator_pool.front().tick)) {
        ComPtr<ID3D12CommandAllocator> allocator = std::move(allocator_pool.front().allocator);
        allocator_pool.pop_front();
        ThrowIfFailed(allocator->Reset(), "ID3D12CommandAllocator::Reset");
        return allocator;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ThrowIfFailed(device.Get()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&allocator)),
                  "CreateCommandAllocator");
    return allocator;
}

} // namespace D3D12
