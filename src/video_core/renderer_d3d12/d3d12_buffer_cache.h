// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <vector>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"

namespace D3D12 {

class Scheduler;
class BufferCacheRuntime;
class GuestDescriptorQueue;

class Buffer : public VideoCommon::BufferBase {
public:
    explicit Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams);
    /// sparse_compatible is ignored: D3D12 buffers here are committed, not reserved resources.
    explicit Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr, u64 size_bytes,
                    bool sparse_compatible = false);
    /// The resource outlives the Buffer until the GPU is done with it.
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) noexcept = default;
    Buffer& operator=(Buffer&&) noexcept;

    [[nodiscard]] ID3D12Resource* Handle() const noexcept { return buffer.Get(); }
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Address() const noexcept {
        return buffer ? buffer->GetGPUVirtualAddress() : 0;
    }
    operator ID3D12Resource*() const noexcept { return Handle(); }
    bool IsRegionUsed(u64 offset, u64 size) const noexcept { return tracker.IsUsed(offset, size); }
    void MarkUsage(u64 offset, u64 size) noexcept { tracker.Track(offset, size); }
    void ResetUsageTracking() noexcept { tracker.Reset(); }

    /// Records a barrier to next unless the buffer is already there (a UAV barrier for UAV ->
    /// UAV). Buffers decay to COMMON at the end of every ExecuteCommandLists, so the tracked state
    /// only holds within the list that set it.
    void Transition(D3D12_RESOURCE_STATES next);
    [[nodiscard]] D3D12_RESOURCE_STATES State() const noexcept { return state; }

private:
    Scheduler* scheduler{};
    ComPtr<ID3D12Resource> buffer;
    VideoCommon::UsageTracker tracker;
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    u64 state_tick{}; ///< scheduler tick of the list that set state
};

/// A buffer bound for a traced draw (RasterizerD3D12::SetDrawTrace), checked against guest memory.
struct TracedBuffer {
    enum class Kind { Uniform, NullUniform, StreamedUniform, Vertex, Index, RewrittenIndex };
    Kind kind{};
    u32 slot{};
    Buffer* buffer{}; ///< null for staging memory and null bindings
    u32 offset{};
    u32 size{};
    u32 stride{}; ///< vertex stride or index size
    DAddr device_addr{};
    const u8* mapped{}; ///< what the GPU reads of a streamed cbuf
};

class BufferCacheRuntime {
    friend Buffer;
    using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

public:
    BufferCacheRuntime(const Device& device, Scheduler& scheduler, StagingBufferPool& staging,
                       TransferBufferPool& transfer_buffers,
                       Tegra::MaxwellDeviceMemoryManager& device_memory);

    /// Where guest uniform, storage and texel buffer views go (set by the rasterizer).
    void SetDescriptorQueue(GuestDescriptorQueue* queue) noexcept { descriptor_queue = queue; }
    /// Collects the buffers of the next bindings (null stops); diagnostics only.
    void SetTraceBuffers(std::vector<TracedBuffer>* out) noexcept {
        trace_buffers = out;
        traced_uniforms = 0;
    }

    void TickFrame(Common::SlotVector<Buffer>& buffers) noexcept;

    /// GPU progress, used by the generic cache to avoid overwriting buffers still in flight.
    u64 CurrentTick();
    bool IsFree(u64 tick);
    void Wait(u64 tick);
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

    /// Index and vertex buffers are collected here and set by ApplyGeometry() right before the
    /// draw, so a flush between binding and drawing (descriptor ring or staging exhaustion)
    /// cannot leave the new command list without them.
    void BindIndexBuffer(PrimitiveTopology topology, IndexFormat format, u32 first,
                         u32 num_indices, Buffer& buffer, u32 offset, u32 size);
    /// Non-indexed quads are rewritten by EmulateTopology, like fans and loops.
    void BindQuadIndexBuffer(PrimitiveTopology, u32, u32) {}
    void BindVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size, u32 stride);
    void BindVertexBuffer(u32 index, ID3D12Resource* buffer, u32 offset, u32 size, u32 stride);
    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings);
    void BindTransformFeedbackBuffer(u32, Buffer&, u32, u32) {}
    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>&) {}
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size);
    void BindUniformBuffer(Buffer& buffer, u32 offset, u32 size);
    void BindStorageBuffer(Buffer& buffer, u32 offset, u32 size, bool is_written);
    void BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat format);
    void BindImageBuffer(Buffer& buffer, u32 offset, u32 size,
                         VideoCore::Surface::PixelFormat format);
    bool ShouldLimitDynamicStorageBuffers() const { return false; }
    u32 GetMaxDynamicStorageBuffers() const { return UINT32_MAX; }

    /// Topologies D3D12 lacks (quads, quad strips, fans, polygons, line loops), drawn without an
    /// index buffer: builds the equivalent list of vertex numbers first..first+count.
    void EmulateTopology(PrimitiveTopology topology, u32 first, u32 count);

    /// Index count of the rewritten index buffer when the draw was rewritten (emulated topology or
    /// 8-bit indices): the draw then reads it from index 0, with the guest's base vertex.
    [[nodiscard]] std::optional<u32> RewrittenIndexCount() const noexcept {
        return rewritten_count;
    }

    /// Sets the collected index and vertex buffers on the command list and forgets them.
    void ApplyGeometry(ID3D12GraphicsCommandList* cmd);

    [[nodiscard]] static bool IsEmulatedTopology(PrimitiveTopology topology) noexcept;

private:
    /// Uploads indices to staging memory and binds them as a 32-bit or 16-bit index buffer.
    void BindRewrittenIndices(std::span<const u32> indices, bool wide);
    ComPtr<ID3D12Resource> CreateDefaultBuffer(u64 size);
    void Copy(Buffer* dst, ID3D12Resource* dst_raw, Buffer* src, ID3D12Resource* src_raw,
              std::span<const VideoCommon::BufferCopy> copies);

    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    TransferBufferPool& transfer_buffers; ///< scratch for copies within one buffer
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    GuestDescriptorQueue* descriptor_queue{};
    std::vector<TracedBuffer>* trace_buffers{};
    u32 traced_uniforms{};

    std::optional<D3D12_INDEX_BUFFER_VIEW> pending_index;
    std::array<D3D12_VERTEX_BUFFER_VIEW, VideoCommon::NUM_VERTEX_BUFFERS> pending_vertex{};
    u32 pending_vertex_mask{};
    std::optional<u32> rewritten_count;
    std::vector<u32> rewrite_scratch;
    std::vector<u8> guest_indices;
    bool logged_rewrite{};
    bool logged_inline_rewrite{};
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
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = true;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;

} // namespace D3D12
