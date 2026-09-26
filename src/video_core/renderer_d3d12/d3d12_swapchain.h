// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <dxgi1_4.h>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

/// Flip-model swapchain on a UWP CoreWindow (the only presentation surface a UWP app on Xbox has).
class Swapchain {
public:
    static constexpr u32 IMAGE_COUNT = 3;
    static constexpr DXGI_FORMAT FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

    /// core_window is the CoreWindow's IUnknown, as winrt::get_unknown hands it out.
    Swapchain(const Device& device, IUnknown* core_window, u32 width, u32 height);

    u32 Width() const {
        return width;
    }
    u32 Height() const {
        return height;
    }
    u32 CurrentIndex() const;
    ID3D12Resource* Image(u32 index) const {
        return images[index].Get();
    }
    void Present();

private:
    ComPtr<IDXGISwapChain3> swapchain;
    std::array<ComPtr<ID3D12Resource>, IMAGE_COUNT> images;
    u32 width;
    u32 height;
};

} // namespace D3D12
