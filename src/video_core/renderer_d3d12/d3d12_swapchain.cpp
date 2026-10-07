// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <dxgi1_3.h>

#include "common/logging.h"
#include "video_core/frame_trace.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"

namespace D3D12 {

namespace {

/// A wait longer than this means the swapchain stopped presenting (a suspended app, a lost
/// device): give up on the wait and let Present report what happened.
constexpr DWORD FRAME_WAIT_TIMEOUT_MS = 1000;

} // Anonymous namespace

Swapchain::Swapchain(const Device& device, IUnknown* core_window, u32 width_, u32 height_)
    : width{width_}, height{height_} {
    DXGI_SWAP_CHAIN_DESC1 desc{
        .Width = width,
        .Height = height,
        .Format = FORMAT,
        .Stereo = FALSE,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = IMAGE_COUNT,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
        .Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
    };
    ComPtr<IDXGISwapChain1> swapchain1;
    HRESULT hr = device.Factory()->CreateSwapChainForCoreWindow(device.Queue(), core_window, &desc,
                                                                nullptr, &swapchain1);
    if (FAILED(hr)) {
        LOG_WARNING(Render, "D3D12: waitable swapchain rejected (0x{:08X}); Present will block",
                    static_cast<u32>(hr));
        desc.Flags = 0;
        hr = device.Factory()->CreateSwapChainForCoreWindow(device.Queue(), core_window, &desc,
                                                            nullptr, &swapchain1);
    }
    ThrowIfFailed(hr, "CreateSwapChainForCoreWindow");
    ThrowIfFailed(swapchain1.As(&swapchain), "IDXGISwapChain3 query");
    if (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        // A waitable swapchain keeps its own latency, not the device's (IDXGIDevice1).
        if (FAILED(swapchain->SetMaximumFrameLatency(MAX_FRAME_LATENCY))) {
            LOG_WARNING(Render, "D3D12: SetMaximumFrameLatency({}) failed", MAX_FRAME_LATENCY);
        }
        frame_latency_waitable = swapchain->GetFrameLatencyWaitableObject();
    }
    for (u32 i = 0; i < IMAGE_COUNT; ++i) {
        ThrowIfFailed(swapchain->GetBuffer(i, IID_PPV_ARGS(&images[i])),
                      "IDXGISwapChain::GetBuffer");
    }
    LOG_INFO(Render, "D3D12: swapchain {}x{}, {} images, frame latency {}", width, height,
             IMAGE_COUNT,
             frame_latency_waitable ? fmt::format("{} (waitable)", MAX_FRAME_LATENCY)
                                    : std::string("default (Present blocks)"));
}

Swapchain::~Swapchain() {
    if (frame_latency_waitable != nullptr) {
        CloseHandle(frame_latency_waitable);
    }
}

u32 Swapchain::CurrentIndex() const {
    return swapchain->GetCurrentBackBufferIndex();
}

void Swapchain::WaitForFrame() {
    if (frame_latency_waitable == nullptr) {
        return;
    }
    const VideoCore::FrameTrace::ScopedSpan trace_wait{
        VideoCore::FrameTrace::Event::HostPresentLong, 1};
    // Not alertable: an APC ending the wait early would let Present block.
    if (WaitForSingleObjectEx(frame_latency_waitable, FRAME_WAIT_TIMEOUT_MS, FALSE) ==
        WAIT_TIMEOUT) {
        LOG_WARNING(Render, "D3D12: the swapchain accepted no frame for {} ms",
                    FRAME_WAIT_TIMEOUT_MS);
    }
}

void Swapchain::Present() {
    const VideoCore::FrameTrace::ScopedSpan trace_present{
        VideoCore::FrameTrace::Event::HostPresentLong, 0};
    ThrowIfFailed(swapchain->Present(1, 0), "IDXGISwapChain::Present");
}

} // namespace D3D12
