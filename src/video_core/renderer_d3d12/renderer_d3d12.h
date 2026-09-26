// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"

namespace D3D12 {

/// Direct3D 12 renderer: a device and swapchain on the UWP CoreWindow, presenting the guest's
/// display framebuffer. The framebuffer is deswizzled on the CPU and drawn to the window with
/// Eden's own blit shaders, translated SPIR-V -> DXIL at runtime (phase 2). If that shader path is
/// unavailable, the phase-1 CPU scale + copy is used instead. Guest GPU work is routed through the
/// D3D12 rasterizer and its phase-3 caches, queries, DMA and fence manager.
///
/// Presentation already runs on the rasterizer infrastructure (phase 3a): the scheduler's command
/// list and ticks, staging memory from the stream buffer, and descriptors from the shader-visible
/// ring and the sampler heap.
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
    /// Builds the blit root signature and PSO from translated SPIR-V; false (logged) on failure.
    bool CreateBlitPipeline();
    /// (Re)creates the guest texture and its SRV for a new size.
    void PrepareGuestImage(u32 width, u32 height);

    /// Deswizzles the guest layer into `linear`; false if there is nothing to show.
    bool ReadGuestLayer(const Tegra::FramebufferConfig& framebuffer);
    /// Converts the cropped, flipped guest image to RGBA8 at its own size (shader path).
    void CopyGuestImage(const Tegra::FramebufferConfig& framebuffer, u8* dst, u32 row_pitch);
    /// Converts and letterboxes the guest image into the window-sized upload image (CPU path).
    void ScaleGuestImage(const Tegra::FramebufferConfig& framebuffer, u8* dst, u32 row_pitch);

    /// Records the shader-path upload + draw of the guest texture into the back buffer.
    void RecordBlit(const StagingBufferRef& upload, ID3D12Resource* image, u32 image_index,
                    D3D12_GPU_DESCRIPTOR_HANDLE srv_table,
                    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table);
    /// Records the CPU-path copy of the upload image into the back buffer.
    void RecordCopy(const StagingBufferRef& upload, ID3D12Resource* image);
    /// Submits the recorded frame and presents back buffer image_index.
    void Present(u32 image_index);

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;
    Device device;
    Swapchain swapchain;
    ShaderCompiler shader_compiler;
    Scheduler scheduler;
    StagingBufferPool staging_pool;
    BufferCacheRuntime buffer_cache_runtime;
    CpuDescriptorAllocator view_descriptors;    ///< offline CBV/SRV/UAV
    CpuDescriptorAllocator sampler_descriptors; ///< offline samplers
    CpuDescriptorAllocator rtv_descriptors;
    CpuDescriptorAllocator dsv_descriptors;
    TextureCacheRuntime texture_cache_runtime;
    DescriptorRing descriptor_ring;
    SamplerHeap sampler_heap;
    RasterizerD3D12 rasterizer;

    std::array<u64, Swapchain::IMAGE_COUNT> present_ticks{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, Swapchain::IMAGE_COUNT> back_buffer_rtvs{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; ///< window-sized image (CPU path)
    u64 upload_size{};

    // Shader path
    bool blit_ready{};
    ComPtr<ID3D12RootSignature> blit_root_signature;
    ComPtr<ID3D12PipelineState> blit_pipeline;
    D3D12_CPU_DESCRIPTOR_HANDLE linear_sampler{};
    ComPtr<ID3D12Resource> guest_texture;
    D3D12_CPU_DESCRIPTOR_HANDLE guest_srv{};
    u32 guest_width{};
    u32 guest_height{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT guest_footprint{};
    u64 guest_upload_size{};

    std::vector<u8> linear; ///< deswizzled guest framebuffer
    int crop_left{};
    int crop_top{};
    int crop_width{};
    int crop_height{};
    bool present_failed{};
};

} // namespace D3D12
