// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <exception>
#include <stdexcept>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

Scheduler::Scheduler(Device& device_) : device{device_} {
    ID3D12Device* const dev = device.Get();
    ThrowIfFailed(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                  "CreateFence (scheduler)");
    fence_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (fence_event == nullptr) {
        throw std::runtime_error("D3D12: CreateEventExW failed");
    }
    current_allocator = AcquireAllocator();
    ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         current_allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&command_list)),
                  "CreateCommandList");
}

Scheduler::~Scheduler() {
    try {
        Finish();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "{}", e.what());
    }
    pending_releases.clear();
    if (fence_event != nullptr) {
        CloseHandle(fence_event);
    }
}

u64 Scheduler::Flush() {
    if (on_submit) {
        on_submit();
    }
    ThrowIfFailed(command_list->Close(), "ID3D12GraphicsCommandList::Close");
    ID3D12CommandList* const lists[] = {command_list.Get()};
    device.Queue()->ExecuteCommandLists(1, lists);

    const u64 signaled = current_tick++;
    ThrowIfFailed(device.Queue()->Signal(fence.Get(), signaled), "ID3D12CommandQueue::Signal");
    allocator_pool.push_back({std::move(current_allocator), signaled});

    CollectGarbage();
    current_allocator = AcquireAllocator();
    ThrowIfFailed(command_list->Reset(current_allocator.Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset");
    return signaled;
}

void Scheduler::Finish() {
    Wait(Flush());
    CollectGarbage();
}

void Scheduler::Wait(u64 tick) {
    if (tick >= current_tick) {
        Flush();
    }
    if (IsFree(tick)) {
        return;
    }
    ThrowIfFailed(fence->SetEventOnCompletion(tick, fence_event),
                  "ID3D12Fence::SetEventOnCompletion");
    WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
    known_gpu_tick = std::max(known_gpu_tick, tick);
}

u64 Scheduler::KnownGpuTick() const {
    const u64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        // GetCompletedValue returns all ones once the device is removed.
        throw std::runtime_error(fmt::format("D3D12: device removed (reason 0x{:08X})",
                                             static_cast<u32>(
                                                 device.Get()->GetDeviceRemovedReason())));
    }
    known_gpu_tick = std::max(known_gpu_tick, completed);
    return known_gpu_tick;
}

void Scheduler::DeferRelease(ComPtr<IUnknown> object) {
    if (object) {
        pending_releases.emplace_back(current_tick, std::move(object));
    }
}

void Scheduler::CollectGarbage() {
    const u64 gpu_tick = KnownGpuTick();
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
