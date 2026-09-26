// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"
#include "video_core/renderer_d3d12/d3d12_indirect_buffer.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"

namespace Tegra {
struct FramebufferConfig;
}

namespace D3D12 {

class AccelerateDMA final : public Tegra::Engines::AccelerateDMAInterface {
public:
    AccelerateDMA(BufferCache& buffer_cache, TextureCache& texture_cache);
    bool BufferCopy(GPUVAddr src, GPUVAddr dst, u64 amount) override;
    bool BufferClear(GPUVAddr address, u64 amount, u32 value) override;
    bool ImageToBuffer(const Tegra::DMA::ImageCopy&, const Tegra::DMA::ImageOperand&,
                       const Tegra::DMA::BufferOperand&) override;
    bool BufferToImage(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                       const Tegra::DMA::ImageOperand&) override;

private:
    template <bool IS_UPLOAD>
    bool BufferImageCopy(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                         const Tegra::DMA::ImageOperand&);
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
};

class RasterizerD3D12 final : public VideoCore::RasterizerInterface,
                              protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    RasterizerD3D12(Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& device_memory,
                    const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                    BufferCacheRuntime& buffer_runtime, TextureCacheRuntime& texture_runtime,
                    DescriptorRing& descriptor_ring, SamplerHeap& sampler_heap,
                    BlitImageHelper& blit_helper, StagingBufferPool& staging);
    ~RasterizerD3D12() override;

    void Draw(bool, u32) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override;
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override;
    void DisableGraphicsUniformBuffer(size_t, u32) override;
    void FlushAll() override;
    void FlushRegion(DAddr, u64, VideoCommon::CacheType = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr, u64,
                         VideoCommon::CacheType = VideoCommon::CacheType::All) override;
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr, u64) override;
    void InvalidateRegion(DAddr, u64,
                          VideoCommon::CacheType = VideoCommon::CacheType::All) override;
    void OnCacheInvalidation(PAddr, u64) override;
    bool OnCPUWrite(PAddr, u64) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr, u64) override;
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override;
    void SignalFence(std::function<void()>&&) override;
    void SyncOperation(std::function<void()>&&) override;
    void SignalSyncPoint(u32) override;
    void SignalReference() override;
    void ReleaseFences(bool = true) override;
    void FlushAndInvalidateRegion(DAddr, u64,
                                  VideoCommon::CacheType = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface&,
                               const Tegra::Engines::Fermi2D::Surface&,
                               const Tegra::Engines::Fermi2D::Config&) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override;
    void LoadDiskResources(u64, std::stop_token,
                           const VideoCore::DiskResourceLoadCallback&) override;
    void InitializeChannel(Tegra::Control::ChannelState&) override;
    void BindChannel(Tegra::Control::ChannelState&) override;
    void ReleaseChannel(s32) override;

    [[nodiscard]] bool AnyCommandQueued() const noexcept { return true; }

    /// A guest image the display can sample directly (see RendererD3D12::Composite).
    struct DisplayTexture {
        D3D12_CPU_DESCRIPTOR_HANDLE srv;
        u32 width;
        u32 height;
    };

    /// The texture cache image holding the framebuffer at framebuffer_addr, transitioned for
    /// sampling, or nullopt when the guest wrote it from the CPU (as RasterizerVulkan).
    std::optional<DisplayTexture> AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                                    DAddr framebuffer_addr);

    /// Logs every draw, clear and skipped draw until turned off: one frame of it, taken on two
    /// machines, shows where their output starts to differ.
    void SetDrawTrace(bool enabled) noexcept {
        trace_draws = enabled;
        trace_index = 0;
    }

private:
    struct DrawParams {
        u32 num_vertices;
        u32 num_instances;
        u32 first_index;
        u32 base_vertex;
        u32 base_instance;
        bool is_indexed;
        u32 runtime_first_vertex; ///< what gl_VertexIndex adds to SV_VertexID
    };
    struct ViewportState {
        u32 yz_flip_mask;
        float width;
        float height;
    };

    using IndirectParams = Tegra::Engines::Maxwell3D::DrawManager::IndirectParams;

    /// Records the complete state of a draw, then the draw. Nothing is carried over from earlier
    /// draws: the state tracker of the Vulkan backend is left for later, when it pays off.
    void RecordDraw(const GraphicsPipeline& pipeline, const PipelineBindings& bindings,
                    const Framebuffer& framebuffer, const DrawParams& params,
                    Maxwell::PrimitiveTopology topology);
    /// RecordDraw without the draw (ExecuteIndirect replaces it).
    void BindDrawState(const GraphicsPipeline& pipeline, const PipelineBindings& bindings,
                       const Framebuffer& framebuffer, const DrawParams& params,
                       Maxwell::PrimitiveTopology topology);
    /// Every draw binds its vertex buffers again (see Draw).
    void MarkVertexBuffersDirty();
    /// Indirect draws of topologies rewritten on the CPU: reads the arguments and draws directly.
    void DrawIndirectOnCpu(const IndirectParams& params);
    [[nodiscard]] ViewportState ComputeViewports(
        std::array<D3D12_VIEWPORT, Maxwell::NumViewports>& viewports) const;
    ViewportState UpdateViewports(ID3D12GraphicsCommandList* cmd);
    void UpdateScissors(ID3D12GraphicsCommandList* cmd);
    [[nodiscard]] D3D12_RECT ScissorRect(size_t index) const;
    void QueryFallback(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32);
    /// One trace line (SetDrawTrace): what was recorded or why it was skipped.
    void TraceDraw(std::string_view what, const GraphicsPipeline* pipeline,
                   const Framebuffer* framebuffer, std::span<const VideoCommon::ImageViewId> views,
                   u32 vertices, u32 instances);

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Scheduler& scheduler;
    BufferCacheRuntime& buffer_runtime;
    DescriptorRing& descriptor_ring;
    SamplerHeap& sampler_heap;
    BlitImageHelper& blit_helper;
    IndirectArgumentRing indirect_args;
    std::vector<u32> indirect_words; ///< CPU part of the ExecuteIndirect records
    GuestDescriptorQueue descriptor_queue;
    BufferCache buffer_cache;
    TextureCache texture_cache;
    PipelineCache pipeline_cache;
    QueryCache query_cache;
    AccelerateDMA accelerate_dma;
    FenceManager fence_manager;
    bool logged_integer_clear{};
    bool logged_draw_texture{};
    bool logged_first_draw{};
    bool logged_stencil_ref{};
    bool logged_layer_clear{};
    bool logged_first_dispatch{};
    bool logged_indirect_dispatch{};
    bool logged_indirect_draw{};
    bool logged_cpu_indirect_draw{};
    bool logged_byte_count_draw{};
    bool trace_draws{};
    u32 trace_index{};
};

} // namespace D3D12
