// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"

namespace D3D12 {

namespace {

/// Guest query types the D3D12 query cache counts on the host (see d3d12_query_cache.cpp); the
/// rest are written by QueryFallback. Same mapping as the OpenGL backend.
std::optional<VideoCore::QueryType> MaxwellToVideoCoreQuery(VideoCommon::QueryType type) {
    switch (type) {
    case VideoCommon::QueryType::PrimitivesGenerated:
    case VideoCommon::QueryType::VtgPrimitivesOut:
        return VideoCore::QueryType::PrimitivesGenerated;
    case VideoCommon::QueryType::ZPassPixelCount64:
        return VideoCore::QueryType::SamplesPassed;
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
        return VideoCore::QueryType::TfbPrimitivesWritten;
    default:
        return std::nullopt;
    }
}

} // Anonymous namespace

RasterizerD3D12::RasterizerD3D12(Tegra::GPU& gpu_,
                                 Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                 const Device& device, Scheduler& scheduler_,
                                 BufferCacheRuntime& buffer_runtime,
                                 TextureCacheRuntime& texture_runtime)
    : gpu{gpu_}, device_memory{device_memory_}, scheduler{scheduler_},
      buffer_cache{device_memory_, buffer_runtime}, texture_cache{texture_runtime, device_memory_},
      query_cache{*this, device_memory_, device, scheduler_},
      accelerate_dma{buffer_cache, texture_cache},
      fence_manager{*this, gpu_, texture_cache, buffer_cache, query_cache, scheduler_} {
    LOG_INFO(Render, "D3D12: phase 3d rasterizer active (fences, queries, caches and DMA)");
}

RasterizerD3D12::~RasterizerD3D12() = default;

void RasterizerD3D12::UnsupportedDraw(const char* operation) {
    if (!logged_phase4_draw) {
        LOG_WARNING(Render, "D3D12: guest {} skipped until phase 4 pipelines are available",
                    operation);
        logged_phase4_draw = true;
    }
}
void RasterizerD3D12::Draw(bool, u32) { UnsupportedDraw("draw"); }
void RasterizerD3D12::DrawTexture() { UnsupportedDraw("draw texture"); }
void RasterizerD3D12::Clear(u32) { UnsupportedDraw("clear"); }
void RasterizerD3D12::DispatchCompute() { UnsupportedDraw("compute dispatch"); }

void RasterizerD3D12::ResetCounter(VideoCommon::QueryType type) {
    const auto host_type = MaxwellToVideoCoreQuery(type);
    if (host_type) query_cache.ResetCounter(*host_type);
}

void RasterizerD3D12::Query(GPUVAddr address, VideoCommon::QueryType type,
                            VideoCommon::QueryPropertiesFlags flags, u32 payload, u32) {
    const auto host_type = MaxwellToVideoCoreQuery(type);
    if (!host_type) return QueryFallback(address, type, flags, payload);
    const bool timeout = True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout);
    query_cache.Query(address, *host_type,
                      timeout ? std::optional<u64>{gpu.GetTicks()} : std::nullopt);
}

void RasterizerD3D12::QueryFallback(GPUVAddr address, VideoCommon::QueryType type,
                                    VideoCommon::QueryPropertiesFlags flags, u32 payload) {
    if (!gpu_memory) return;
    if (type != VideoCommon::QueryType::Payload) payload = 1;
    auto operation = [this, address, flags, payload, memory = gpu_memory] {
        if (True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout)) {
            memory->Write<u64>(address + 8, gpu.GetTicks());
            memory->Write<u64>(address, payload);
        } else {
            memory->Write<u32>(address, payload);
        }
    };
    if (True(flags & VideoCommon::QueryPropertiesFlags::IsAFence)) {
        SignalFence(std::move(operation));
    } else {
        operation();
    }
}

void RasterizerD3D12::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr address,
                                                u32 size) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.BindGraphicsUniformBuffer(stage, index, address, size);
}
void RasterizerD3D12::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}
void RasterizerD3D12::FlushAll() { scheduler.Finish(); }

void RasterizerD3D12::FlushRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (!address || !size) return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::QueryCache)) query_cache.FlushRegion(address, size);
}

bool RasterizerD3D12::MustFlushRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(address, size)) return true;
    }
    if (Settings::IsGPULevelHigh() && True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(address, size);
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerD3D12::GetFlushArea(DAddr address, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        if (auto area = texture_cache.GetFlushArea(address, size)) return *area;
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (auto area = buffer_cache.GetFlushArea(address, size)) return *area;
    }
    return {.start_address = Common::AlignDown(address, Core::DEVICE_PAGESIZE),
            .end_address = Common::AlignUp(address + size, Core::DEVICE_PAGESIZE),
            .preemtive = true};
}

void RasterizerD3D12::InvalidateRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (!address || !size) return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::QueryCache)) query_cache.InvalidateRegion(address, size);
}

bool RasterizerD3D12::OnCPUWrite(PAddr address, u64 size) {
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(address, size)) return true;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(address, size);
    }
    return false;
}

void RasterizerD3D12::OnCacheInvalidation(PAddr address, u64 size) {
    InvalidateRegion(address, size, VideoCommon::CacheType::TextureCache |
                                        VideoCommon::CacheType::BufferCache);
}
void RasterizerD3D12::InvalidateGPUCache() { gpu.InvalidateGPUCache(); }
void RasterizerD3D12::UnmapMemory(DAddr address, u64 size) {
    { std::scoped_lock lock{texture_cache.mutex}; texture_cache.UnmapMemory(address, size); }
    { std::scoped_lock lock{buffer_cache.mutex}; buffer_cache.WriteMemory(address, size); }
}
void RasterizerD3D12::ModifyGPUMemory(size_t as_id, GPUVAddr address, u64 size) {
    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UnmapGPUMemory(as_id, address, size);
}
void RasterizerD3D12::SignalFence(std::function<void()>&& f) { fence_manager.SignalFence(std::move(f)); }
void RasterizerD3D12::SyncOperation(std::function<void()>&& f) { fence_manager.SyncOperation(std::move(f)); }
void RasterizerD3D12::SignalSyncPoint(u32 value) { fence_manager.SignalSyncPoint(value); }
void RasterizerD3D12::SignalReference() { fence_manager.SignalOrdering(); }
void RasterizerD3D12::ReleaseFences(bool force) { fence_manager.WaitPendingFences(force); }
void RasterizerD3D12::FlushAndInvalidateRegion(DAddr a, u64 s, VideoCommon::CacheType w) {
    if (Settings::IsGPULevelHigh()) FlushRegion(a, s, w);
    InvalidateRegion(a, s, w);
}
void RasterizerD3D12::WaitForIdle() { scheduler.Finish(); }
void RasterizerD3D12::FragmentBarrier() { scheduler.Flush(); }
void RasterizerD3D12::TiledCacheBarrier() { scheduler.Flush(); }
void RasterizerD3D12::FlushCommands() { scheduler.Flush(); }
void RasterizerD3D12::TickFrame() {
    fence_manager.TickFrame();
    { std::scoped_lock lock{texture_cache.mutex}; texture_cache.TickFrame(); }
    { std::scoped_lock lock{buffer_cache.mutex}; buffer_cache.TickFrame(); }
}
bool RasterizerD3D12::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                            const Tegra::Engines::Fermi2D::Surface& dst,
                                            const Tegra::Engines::Fermi2D::Config& config) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, config);
}
Tegra::Engines::AccelerateDMAInterface& RasterizerD3D12::AccessAccelerateDMA() { return accelerate_dma; }
void RasterizerD3D12::AccelerateInlineToMemory(GPUVAddr address, size_t size,
                                               std::span<const u8> memory) {
    const auto cpu_address = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_address) return gpu_memory->WriteBlock(address, memory.data(), size);
    gpu_memory->WriteBlockUnsafe(address, memory.data(), size);
    { std::scoped_lock lock{buffer_cache.mutex};
      if (!buffer_cache.InlineMemory(*cpu_address, size, memory)) buffer_cache.WriteMemory(*cpu_address, size); }
    { std::scoped_lock lock{texture_cache.mutex}; texture_cache.WriteMemory(*cpu_address, size); }
    query_cache.InvalidateRegion(*cpu_address, size);
}
void RasterizerD3D12::LoadDiskResources(u64, std::stop_token,
                                        const VideoCore::DiskResourceLoadCallback&) {}
void RasterizerD3D12::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.CreateChannel(channel); buffer_cache.CreateChannel(channel); }
    query_cache.CreateChannel(channel);
}
void RasterizerD3D12::BindChannel(Tegra::Control::ChannelState& channel) {
    BindToChannel(channel.bind_id);
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.BindToChannel(channel.bind_id); buffer_cache.BindToChannel(channel.bind_id); }
    query_cache.BindToChannel(channel.bind_id);
}
void RasterizerD3D12::ReleaseChannel(s32 id) {
    EraseChannel(id);
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.EraseChannel(id); buffer_cache.EraseChannel(id); }
    query_cache.EraseChannel(id);
}

AccelerateDMA::AccelerateDMA(BufferCache& buffers, TextureCache& textures)
    : buffer_cache{buffers}, texture_cache{textures} {}
bool AccelerateDMA::BufferCopy(GPUVAddr src, GPUVAddr dst, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex}; return buffer_cache.DMACopy(src, dst, amount);
}
bool AccelerateDMA::BufferClear(GPUVAddr address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex}; return buffer_cache.DMAClear(address, amount, value);
}
template <bool IS_UPLOAD>
bool AccelerateDMA::BufferImageCopy(const Tegra::DMA::ImageCopy& info,
                                    const Tegra::DMA::BufferOperand& buffer_operand,
                                    const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) return false;
    const u32 size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    constexpr auto sync = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto op = IS_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                              : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(buffer_operand.address, size, sync, op);
    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        info, buffer_operand, image_operand, image_id, IS_UPLOAD);
    const std::span copies{&copy, 1};
    if constexpr (IS_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copies);
    } else {
        if (offset % VideoCore::Surface::BytesPerBlock(image->info.format)) return false;
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copies,
                                              buffer_operand.address, size);
    }
    return true;
}
bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& i,
                                  const Tegra::DMA::ImageOperand& image,
                                  const Tegra::DMA::BufferOperand& buffer) {
    return BufferImageCopy<false>(i, buffer, image);
}
bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& i,
                                  const Tegra::DMA::BufferOperand& buffer,
                                  const Tegra::DMA::ImageOperand& image) {
    return BufferImageCopy<true>(i, buffer, image);
}

} // namespace D3D12
