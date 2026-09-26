// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace D3D12 {

/// Direct3D 12 renderer, phase 1: a real device and swapchain on the UWP CoreWindow, presenting the
/// guest's display framebuffer through a CPU deswizzle + copy. There is no shader path yet, so
/// guest GPU work still goes to the null rasterizer; homebrew that draws with the CPU (libnx
/// framebuffer / console) is displayed as-is.
class RendererD3D12 final : public VideoCore::RendererBase {
public:
    explicit RendererD3D12(Core::Frontend::EmuWindow& emu_window,
                           Tegra::MaxwellDeviceMemoryManager& device_memory, Tegra::GPU& gpu,
                           std::unique_ptr<Core::Frontend::GraphicsContext> context);
    ~RendererD3D12() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.AdapterName();
    }

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12Resource> upload;
        u8* mapped{};
        u64 fence_value{};
    };

    /// Converts the guest layer into the RGBA8 upload image, letterboxed into the window layout.
    void DrawLayer(const Tegra::FramebufferConfig& framebuffer, u8* dst);
    /// Copies frame image_index's upload image to the swapchain and presents it.
    void Present(u32 image_index);

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;
    Device device;
    Swapchain swapchain;
    Null::RasterizerNull rasterizer;

    ComPtr<ID3D12GraphicsCommandList> command_list;
    std::array<Frame, Swapchain::IMAGE_COUNT> frames;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 upload_size{};

    std::vector<u8> linear; ///< deswizzled guest framebuffer
    bool present_failed{};
};

} // namespace D3D12
