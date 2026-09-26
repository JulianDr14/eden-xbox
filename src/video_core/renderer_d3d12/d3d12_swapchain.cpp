// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"

namespace D3D12 {

Swapchain::Swapchain(const Device& device, IUnknown* core_window, u32 width_, u32 height_)
    : width{width_}, height{height_} {
    const DXGI_SWAP_CHAIN_DESC1 desc{
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
        .Flags = 0,
    };
    ComPtr<IDXGISwapChain1> swapchain1;
    ThrowIfFailed(device.Factory()->CreateSwapChainForCoreWindow(device.Queue(), core_window, &desc,
                                                                 nullptr, &swapchain1),
                  "CreateSwapChainForCoreWindow");
    ThrowIfFailed(swapchain1.As(&swapchain), "IDXGISwapChain3 query");
    for (u32 i = 0; i < IMAGE_COUNT; ++i) {
        ThrowIfFailed(swapchain->GetBuffer(i, IID_PPV_ARGS(&images[i])),
                      "IDXGISwapChain::GetBuffer");
    }
    LOG_INFO(Render, "D3D12: swapchain {}x{}, {} images", width, height, IMAGE_COUNT);
}

u32 Swapchain::CurrentIndex() const {
    return swapchain->GetCurrentBackBufferIndex();
}

void Swapchain::Present() {
    ThrowIfFailed(swapchain->Present(1, 0), "IDXGISwapChain::Present");
}

} // namespace D3D12
