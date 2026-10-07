// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include "common/polyfill_thread.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"

namespace D3D12 {

class CpuDescriptorAllocator;
class PresentManager;
class Scheduler;

/// An image the renderer composites one frame into, sized and formatted like the back buffers.
struct PresentFrame {
    ComPtr<ID3D12Resource> image;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    /// State of image after the commands recorded so far; COPY_SOURCE while it waits to be
    /// presented.
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COPY_SOURCE};
    /// Scheduler tick that submitted the frame's rendering.
    u64 render_tick{};
    u32 index{};

    /// Records a transition to next on cmd unless image is already there.
    void Transition(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES next);
};

/// A frame lent to the renderer: it goes back to the pool when the lease ends without a Present
/// (a composition that threw), so a failed frame never starves the pool.
class FrameLease {
public:
    FrameLease() = default;
    FrameLease(PresentManager& manager, PresentFrame& frame) : manager{&manager}, frame{&frame} {}
    ~FrameLease();

    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    PresentFrame& operator*() const noexcept {
        return *frame;
    }
    PresentFrame* operator->() const noexcept {
        return frame;
    }

private:
    friend PresentManager;
    PresentManager* manager{};
    PresentFrame* frame{};
};

/// Presents composited frames, on a thread of its own when it can (Vulkan's PresentManager).
///
/// The renderer composites each frame into a PresentFrame and hands it over with Present(). The
/// present thread waits until the swapchain accepts a frame (its frame latency waitable object),
/// copies the frame into the back buffer with a command list of its own and presents it. The GPU
/// thread never blocks on the display: it only waits when every frame is still queued.
///
/// Synchronization:
/// - GPU order: both threads submit to the one direct queue, and a frame is queued only after the
///   scheduler submitted its rendering, so its copy executes after it. A frame is lent again only
///   after its copy was submitted, so the next rendering into it executes after the copy too.
/// - DXGI submits to the queue inside Present, and Present racing ExecuteCommandLists on another
///   thread can deadlock: both go through Device::QueueMutex. The present thread waits for the
///   swapchain before taking it, so Present does not block while holding it.
/// - CPU reuse: a frame's copy allocator is reset only once the copy fence has passed it, and a
///   lent frame waits for the GPU to finish the rendering it held before (at most FRAME_COUNT
///   frames ahead of the GPU).
///
/// Without a waitable swapchain (Present would block holding the queue lock) or with
/// async_presentation off, the copy and Present run on the caller's thread instead.
class PresentManager {
public:
    static constexpr u32 FRAME_COUNT = Swapchain::IMAGE_COUNT;

    PresentManager(const Device& device, Scheduler& scheduler, Swapchain& swapchain,
                   CpuDescriptorAllocator& rtv_descriptors, bool async_presentation);
    ~PresentManager();

    PresentManager(const PresentManager&) = delete;
    PresentManager& operator=(const PresentManager&) = delete;

    /// A frame to composite into; blocks while every frame is queued for presentation.
    [[nodiscard]] FrameLease AcquireFrame();

    /// Submits the commands recorded so far (they must have rendered the frame) and presents it.
    void Present(FrameLease&& lease);

    /// Blocks until every queued frame has been presented.
    void WaitIdle();

    [[nodiscard]] bool IsAsync() const noexcept {
        return present_thread.joinable();
    }

    /// The error that stopped the present thread, once. Frames presented after it are dropped.
    [[nodiscard]] std::optional<std::string> TakeError();

private:
    friend FrameLease;

    struct CopyContext {
        ComPtr<ID3D12CommandAllocator> allocator;
        u64 fence_value{}; ///< copy fence value of the last copy recorded with it
    };

    void PresentThread(std::stop_token token);
    /// Copies frame into the back buffer and presents it (present thread, or the caller).
    void CopyToSwapchain(PresentFrame& frame);
    void WaitForCopy(u64 value);
    /// Returns frame to the pool.
    void Release(PresentFrame& frame);

    const Device& device;
    Scheduler& scheduler;
    Swapchain& swapchain;
    CpuDescriptorAllocator& rtv_descriptors;

    std::array<PresentFrame, FRAME_COUNT> frames;
    std::array<CopyContext, FRAME_COUNT> copy_contexts;
    ComPtr<ID3D12GraphicsCommandList> copy_list;
    ComPtr<ID3D12Fence> copy_fence;
    u64 copy_fence_value{}; ///< last value signaled, by whoever presents
    HANDLE copy_event{};

    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::deque<PresentFrame*> free_frames;

    std::mutex queue_mutex;
    std::condition_variable_any queue_cv;
    std::condition_variable_any idle_cv;
    std::deque<PresentFrame*> queued_frames;
    u32 frames_in_flight{}; ///< queued or being presented

    std::mutex error_mutex;
    std::optional<std::string> error;
    bool failed{}; ///< present thread only

    /// Last member: stopped and joined before the state it uses goes away.
    std::jthread present_thread;
};

} // namespace D3D12
