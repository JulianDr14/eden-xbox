// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"

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
                    const Device& device, Scheduler& scheduler,
                    BufferCacheRuntime& buffer_runtime, TextureCacheRuntime& texture_runtime);
    ~RasterizerD3D12() override;

    void Draw(bool, u32) override;
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

private:
    void UnsupportedDraw(const char* operation);
    void QueryFallback(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32);

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Scheduler& scheduler;
    BufferCache buffer_cache;
    TextureCache texture_cache;
    QueryCache query_cache;
    AccelerateDMA accelerate_dma;
    FenceManager fence_manager;
    bool logged_phase4_draw{};
};

} // namespace D3D12
