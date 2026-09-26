// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {

class Scheduler;
class BufferCacheRuntime;

class Buffer : public VideoCommon::BufferBase {
public:
    explicit Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams);
    explicit Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr, u64 size_bytes);

    [[nodiscard]] ID3D12Resource* Handle() const noexcept { return buffer.Get(); }
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Address() const noexcept {
        return buffer ? buffer->GetGPUVirtualAddress() : 0;
    }
    operator ID3D12Resource*() const noexcept { return Handle(); }
    bool IsRegionUsed(u64 offset, u64 size) const noexcept { return tracker.IsUsed(offset, size); }
    void MarkUsage(u64 offset, u64 size) noexcept { tracker.Track(offset, size); }
    void ResetUsageTracking() noexcept { tracker.Reset(); }

private:
    ComPtr<ID3D12Resource> buffer;
    VideoCommon::UsageTracker tracker;
};

class BufferCacheRuntime {
    friend Buffer;
    using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

public:
    BufferCacheRuntime(const Device& device, Scheduler& scheduler, StagingBufferPool& staging);
    void RunSelfTest();

    void TickFrame(Common::SlotVector<Buffer>& buffers) noexcept;
    void Finish();
    u64 GetDeviceLocalMemory() const;
    u64 GetDeviceMemoryUsage() const;
    bool CanReportMemoryUsage() const { return true; }
    u32 GetUniformBufferAlignment() const { return D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT; }
    u32 GetStorageBufferAlignment() const { return 16; }

    StagingBufferRef UploadStagingBuffer(size_t size);
    StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);
    void FreeDeferredStagingBuffer(StagingBufferRef& ref);
    bool CanReorderUpload(const Buffer&, std::span<const VideoCommon::BufferCopy>) { return false; }
    void PreCopyBarrier() {}
    void PostCopyBarrier() {}

    void CopyBuffer(Buffer& dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies,
                    bool barrier, bool can_reorder = false);
    void CopyBuffer(Buffer& dst, ID3D12Resource* src,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool can_reorder = false);
    void CopyBuffer(ID3D12Resource* dst, Buffer& src,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool can_reorder = false);
    void ClearBuffer(Buffer& dst, u32 offset, size_t size, u32 value);

    void BindIndexBuffer(PrimitiveTopology topology, IndexFormat format, u32 base_vertex,
                         u32 num_indices, Buffer& buffer, u32 offset, u32 size);
    void BindQuadIndexBuffer(PrimitiveTopology, u32, u32) {}
    void BindVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size, u32 stride);
    void BindVertexBuffer(u32 index, ID3D12Resource* buffer, u32 offset, u32 size, u32 stride);
    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings);
    void BindTransformFeedbackBuffer(u32, Buffer&, u32, u32) {}
    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>&) {}
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size);
    void BindUniformBuffer(Buffer&, u32, u32) {}
    void BindStorageBuffer(Buffer&, u32, u32, bool) {}
    void BindTextureBuffer(Buffer&, u32, u32, VideoCore::Surface::PixelFormat) {}
    bool ShouldLimitDynamicStorageBuffers() const { return false; }
    u32 GetMaxDynamicStorageBuffers() const { return UINT32_MAX; }

private:
    ComPtr<ID3D12Resource> CreateDefaultBuffer(u64 size);
    void Copy(Buffer* dst, ID3D12Resource* dst_raw, Buffer* src, ID3D12Resource* src_raw,
              std::span<const VideoCommon::BufferCopy> copies);

    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging;
};

struct BufferCacheParams {
    using Runtime = BufferCacheRuntime;
    using Buffer = D3D12::Buffer;
    using Async_Buffer = StagingBufferRef;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;
    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;

} // namespace D3D12
