// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/thread.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_present_manager.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

void PresentFrame::Transition(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES next) {
    if (state != next) {
        TransitionResource(cmd, image.Get(), state, next);
        state = next;
    }
}

FrameLease::~FrameLease() {
    if (manager && frame) {
        manager->Release(*frame);
    }
}

FrameLease::FrameLease(FrameLease&& other) noexcept
    : manager{std::exchange(other.manager, nullptr)}, frame{std::exchange(other.frame, nullptr)} {}

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept {
    if (this != &other) {
        if (manager && frame) {
            manager->Release(*frame);
        }
        manager = std::exchange(other.manager, nullptr);
        frame = std::exchange(other.frame, nullptr);
    }
    return *this;
}

PresentManager::PresentManager(const Device& device_, Scheduler& scheduler_,
                               Swapchain& swapchain_, CpuDescriptorAllocator& rtv_descriptors_,
                               bool async_presentation)
    : device{device_}, scheduler{scheduler_}, swapchain{swapchain_},
      rtv_descriptors{rtv_descriptors_} {
    ID3D12Device* const dev = device.Get();
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = swapchain.Width(),
        .Height = swapchain.Height(),
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = Swapchain::FORMAT,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    const D3D12_CLEAR_VALUE clear{.Format = Swapchain::FORMAT, .Color = {0.0f, 0.0f, 0.0f, 1.0f}};
    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        PresentFrame& frame = frames[i];
        frame.index = i;
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   frame.state, &clear,
                                                   IID_PPV_ARGS(&frame.image)),
                      "CreateCommittedResource (present frame)");
        frame.image->SetName((L"Present frame " + std::to_wstring(i)).c_str());
        frame.rtv = rtv_descriptors.Allocate();
        dev->CreateRenderTargetView(frame.image.Get(), nullptr, frame.rtv);
        ThrowIfFailed(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&copy_contexts[i].allocator)),
                      "CreateCommandAllocator (present)");
        free_frames.push_back(&frame);
    }
    ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         copy_contexts[0].allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&copy_list)),
                  "CreateCommandList (present)");
    ThrowIfFailed(copy_list->Close(), "ID3D12GraphicsCommandList::Close (present)");
    ThrowIfFailed(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence)),
                  "CreateFence (present)");
    copy_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (copy_event == nullptr) {
        throw std::runtime_error("D3D12: CreateEventExW failed (present)");
    }

    // Present blocks without a waitable swapchain, and it would block holding the queue lock.
    const bool use_thread = async_presentation && swapchain.HasFrameLatencyWaitable();
    if (use_thread) {
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
    LOG_INFO(Render, "D3D12: presentation {} ({} frames of {}x{})",
             use_thread ? "on its own thread"
                        : (async_presentation ? "on the GPU thread (no waitable swapchain)"
                                              : "on the GPU thread"),
             FRAME_COUNT, swapchain.Width(), swapchain.Height());
}

PresentManager::~PresentManager() {
    try {
        WaitIdle();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: present shutdown: {}", e.what());
    }
    if (present_thread.joinable()) {
        present_thread.request_stop();
        present_thread.join();
    }
    // The last copies may still read the frames on the GPU.
    try {
        WaitForCopy(copy_fence_value);
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: present shutdown: {}", e.what());
    }
    for (PresentFrame& frame : frames) {
        if (frame.rtv.ptr != 0) {
            rtv_descriptors.Free(frame.rtv);
        }
    }
    if (copy_event != nullptr) {
        CloseHandle(copy_event);
    }
}

FrameLease PresentManager::AcquireFrame() {
    PresentFrame* frame{};
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_frames.empty(); });
        frame = free_frames.front();
        free_frames.pop_front();
    }
    // The queue already orders new commands after the frame's previous rendering and copy; this
    // only keeps the CPU from running more than FRAME_COUNT frames ahead of the GPU.
    scheduler.Wait(frame->render_tick);
    return FrameLease{*this, *frame};
}

void PresentManager::Present(FrameLease&& lease) {
    PresentFrame& frame = *lease.frame;
    lease.manager = nullptr; // the frame comes back through Release once presented
    lease.frame = nullptr;
    try {
        frame.Transition(scheduler.CommandList(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        frame.render_tick = scheduler.Flush();
    } catch (...) {
        Release(frame);
        throw;
    }
    if (!IsAsync()) {
        // Release even if the copy throws: the frame holds nothing the next one depends on.
        struct ReleaseOnExit {
            PresentManager& manager;
            PresentFrame& frame;
            ~ReleaseOnExit() {
                manager.Release(frame);
            }
        } release{*this, frame};
        CopyToSwapchain(frame);
        return;
    }
    {
        std::scoped_lock lock{queue_mutex};
        queued_frames.push_back(&frame);
        ++frames_in_flight;
    }
    queue_cv.notify_one();
}

void PresentManager::WaitIdle() {
    if (!IsAsync()) {
        return;
    }
    std::unique_lock lock{queue_mutex};
    idle_cv.wait(lock, [this] { return frames_in_flight == 0; });
}

std::optional<std::string> PresentManager::TakeError() {
    std::scoped_lock lock{error_mutex};
    return std::exchange(error, std::nullopt);
}

void PresentManager::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("D3D12Present");
    // Late presents are visible stutter; the thread sleeps on the swapchain otherwise.
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    while (true) {
        PresentFrame* frame{};
        {
            std::unique_lock lock{queue_mutex};
            if (!queue_cv.wait(lock, token, [this] { return !queued_frames.empty(); })) {
                return; // stop requested
            }
            frame = queued_frames.front();
            queued_frames.pop_front();
        }
        if (!failed) {
            try {
                CopyToSwapchain(*frame);
            } catch (const std::exception& e) {
                // Logged here; the renderer stops presenting on its next frame (TakeError).
                LOG_CRITICAL(Render, "D3D12: present thread: {}", e.what());
                failed = true;
                std::scoped_lock lock{error_mutex};
                error = e.what();
            }
        }
        Release(*frame);
        {
            std::scoped_lock lock{queue_mutex};
            --frames_in_flight;
        }
        idle_cv.notify_all();
    }
}

void PresentManager::CopyToSwapchain(PresentFrame& frame) {
    // Waiting here, before taking the queue lock, is what keeps Present from blocking under it.
    // A frame the swapchain did not accept in time is dropped: presenting it would block.
    if (!swapchain.WaitForFrame()) {
        return;
    }
    CopyContext& context = copy_contexts[frame.index];
    WaitForCopy(context.fence_value);
    ThrowIfFailed(context.allocator->Reset(), "ID3D12CommandAllocator::Reset (present)");
    ThrowIfFailed(copy_list->Reset(context.allocator.Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset (present)");

    ID3D12Resource* const back_buffer = swapchain.Image(swapchain.CurrentIndex());
    TransitionResource(copy_list.Get(), back_buffer, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    copy_list->CopyResource(back_buffer, frame.image.Get());
    TransitionResource(copy_list.Get(), back_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    ThrowIfFailed(copy_list->Close(), "ID3D12GraphicsCommandList::Close (present)");

    std::scoped_lock lock{device.QueueMutex()};
    ID3D12CommandList* const lists[] = {copy_list.Get()};
    device.Queue()->ExecuteCommandLists(1, lists);
    ThrowIfFailed(device.Queue()->Signal(copy_fence.Get(), ++copy_fence_value),
                  "ID3D12CommandQueue::Signal (present)");
    context.fence_value = copy_fence_value;
    swapchain.Present();
}

void PresentManager::WaitForCopy(u64 value) {
    const u64 completed = copy_fence->GetCompletedValue();
    if (completed >= value || completed == UINT64_MAX) {
        return; // done, or the device is gone (the fence then reads all ones)
    }
    ThrowIfFailed(copy_fence->SetEventOnCompletion(value, copy_event),
                  "ID3D12Fence::SetEventOnCompletion (present)");
    WaitForSingleObjectEx(copy_event, INFINITE, FALSE);
}

void PresentManager::Release(PresentFrame& frame) {
    {
        std::scoped_lock lock{free_mutex};
        free_frames.push_back(&frame);
    }
    free_cv.notify_one();
}

} // namespace D3D12
