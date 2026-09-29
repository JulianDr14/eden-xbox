// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/surface.h"

namespace D3D12 {

namespace {

using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

/// Rewrites count vertices of an emulated topology as a list D3D12 can draw; vertex(i) gives the
/// vertex number of the i-th guest vertex. Same triangulation as the Vulkan backend's quad passes.
template <typename Vertex>
void AssembleTopology(PrimitiveTopology topology, u32 count, Vertex&& vertex,
                      std::vector<u32>& out) {
    out.clear();
    switch (topology) {
    case PrimitiveTopology::Quads:
        for (u32 quad = 0; quad + 4 <= count; quad += 4) {
            for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) {
                out.push_back(vertex(quad + i));
            }
        }
        break;
    case PrimitiveTopology::QuadStrip:
        for (u32 base = 0; base + 4 <= count; base += 2) {
            for (const u32 i : {0u, 3u, 1u, 0u, 2u, 3u}) {
                out.push_back(vertex(base + i));
            }
        }
        break;
    case PrimitiveTopology::TriangleFan:
    case PrimitiveTopology::Polygon:
        for (u32 i = 1; i + 1 < count; ++i) {
            out.push_back(vertex(0));
            out.push_back(vertex(i));
            out.push_back(vertex(i + 1));
        }
        break;
    case PrimitiveTopology::LineLoop:
        if (count < 2) {
            break;
        }
        for (u32 i = 0; i + 1 < count; ++i) {
            out.push_back(vertex(i));
            out.push_back(vertex(i + 1));
        }
        out.push_back(vertex(count - 1));
        out.push_back(vertex(0));
        break;
    default:
        for (u32 i = 0; i < count; ++i) {
            out.push_back(vertex(i));
        }
        break;
    }
}

u32 IndexSize(IndexFormat format) {
    switch (format) {
    case IndexFormat::UnsignedByte:
        return 1;
    case IndexFormat::UnsignedShort:
        return 2;
    case IndexFormat::UnsignedInt:
        return 4;
    }
    return 4;
}

} // Anonymous namespace

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams params)
    : VideoCommon::BufferBase(params), scheduler{&runtime.scheduler},
      buffer{runtime.CreateDefaultBuffer(4)}, tracker{4096} {}

Buffer::Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr, u64 size_bytes, bool)
    : VideoCommon::BufferBase(cpu_addr, size_bytes), scheduler{&runtime.scheduler},
      // Whole 256-byte blocks: a guest cbuf ending with the buffer still fits a CBV.
      buffer{runtime.CreateDefaultBuffer(
          Common::AlignUp(size_bytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))},
      tracker{size_bytes} {}

Buffer::~Buffer() {
    if (scheduler && buffer) {
        scheduler->DeferRelease(std::move(buffer));
    }
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (scheduler && buffer) {
            scheduler->DeferRelease(std::move(buffer));
        }
        static_cast<VideoCommon::BufferBase&>(*this) = std::move(other);
        scheduler = other.scheduler;
        buffer = std::move(other.buffer);
        tracker = std::move(other.tracker);
        state = other.state;
        state_tick = other.state_tick;
    }
    return *this;
}

void Buffer::Transition(D3D12_RESOURCE_STATES next) {
    if (!scheduler || !buffer) {
        return;
    }
    const u64 tick = scheduler->CurrentTick();
    const D3D12_RESOURCE_STATES current = state_tick == tick ? state : D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList* const cmd = scheduler->CommandList();
    if (current == next) {
        if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            // Orders the previous draw's or dispatch's writes before this one.
            const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                                 .UAV = {.pResource = buffer.Get()}};
            cmd->ResourceBarrier(1, &barrier);
        } else if (next == D3D12_RESOURCE_STATE_COPY_DEST) {
            // Copies into one resource are unordered without a barrier between them, and the
            // Series runs them in parallel: CreateBuffer's zero fill landed over the data
            // JoinOverlap copies in. A round trip through COMMON waits for the earlier copy.
            const D3D12_RESOURCE_BARRIER barriers[2]{
                {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                 .Transition = {.pResource = buffer.Get(),
                                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                                .StateAfter = D3D12_RESOURCE_STATE_COMMON}},
                {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                 .Transition = {.pResource = buffer.Get(),
                                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                                .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST}},
            };
            cmd->ResourceBarrier(2, barriers);
        }
    } else {
        const D3D12_RESOURCE_BARRIER barrier{
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {.pResource = buffer.Get(),
                           .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           .StateBefore = current,
                           .StateAfter = next}};
        cmd->ResourceBarrier(1, &barrier);
    }
    state = next;
    state_tick = tick;
}

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                       StagingBufferPool& staging_,
                                       Tegra::MaxwellDeviceMemoryManager& device_memory_)
    : device{device_}, scheduler{scheduler_}, staging{staging_}, device_memory{device_memory_} {
    LOG_INFO(Render, "D3D12: buffer cache runtime ready");
}

ComPtr<ID3D12Resource> BufferCacheRuntime::CreateDefaultBuffer(u64 size) {
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                   .Width = size, .Height = 1, .DepthOrArraySize = 1,
                                   .MipLevels = 1, .Format = DXGI_FORMAT_UNKNOWN,
                                   .SampleDesc = {.Count = 1},
                                   .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                   // Storage buffers and image buffers are UAVs.
                                   .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> result;
    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result)),
                  "CreateCommittedResource (buffer cache)");
    CheckRemovedAfter(device.Get(), [&] { return fmt::format("creating a {} byte buffer", size); });
    return result;
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& buffers) noexcept {
    for (auto it = buffers.begin(); it != buffers.end(); ++it) {
        it->ResetUsageTracking();
    }
}
void BufferCacheRuntime::Finish() { scheduler.Finish(); }
u64 BufferCacheRuntime::CurrentTick() { return scheduler.CurrentTick(); }
bool BufferCacheRuntime::IsFree(u64 tick) { return scheduler.IsFree(tick); }
void BufferCacheRuntime::Wait(u64 tick) { scheduler.Wait(tick); }
u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    const u64 budget = device.QueryVideoMemory().Budget;
    return budget != 0 ? budget : 4ULL * 1024 * 1024 * 1024;
}
u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.CacheMemoryUsage();
}
StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) { return staging.Request(size, MemoryUsage::Upload); }
StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) { return staging.Request(size, MemoryUsage::Download, deferred); }
void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) { staging.FreeDeferred(ref); }

void BufferCacheRuntime::Copy(Buffer* dst, ID3D12Resource* dst_raw, Buffer* src,
                              ID3D12Resource* src_raw,
                              std::span<const VideoCommon::BufferCopy> copies) {
    auto* cmd = scheduler.CommandList();
    if (dst_raw == src_raw) {
        // A buffer cannot be COPY_SOURCE and COPY_DEST at once: bounce through a scratch buffer.
        u64 scratch_size = 0;
        for (const auto& copy : copies) {
            scratch_size += copy.size;
        }
        if (scratch_size == 0) {
            return;
        }
        ComPtr<ID3D12Resource> scratch = CreateDefaultBuffer(scratch_size);
        std::vector<VideoCommon::BufferCopy> to_scratch;
        std::vector<VideoCommon::BufferCopy> from_scratch;
        u64 offset = 0;
        for (const auto& copy : copies) {
            to_scratch.push_back({.src_offset = copy.src_offset, .dst_offset = offset,
                                  .size = copy.size});
            from_scratch.push_back({.src_offset = offset, .dst_offset = copy.dst_offset,
                                    .size = copy.size});
            offset += copy.size;
        }
        Copy(nullptr, scratch.Get(), src, src_raw, to_scratch);
        const D3D12_RESOURCE_BARRIER to_source{
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {.pResource = scratch.Get(),
                           .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                           .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE}};
        cmd->ResourceBarrier(1, &to_source);
        Copy(dst, dst_raw, nullptr, scratch.Get(), from_scratch);
        scheduler.DeferRelease(std::move(scratch));
        return;
    }
    // Cache buffers are tracked (Buffer::Transition). Raw resources are staging memory, whose
    // state never changes, or the scratch buffer above, promoted from COMMON.
    if (dst) dst->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    if (src) src->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    for (const auto& copy : copies) {
        cmd->CopyBufferRegion(dst_raw, copy.dst_offset, src_raw, copy.src_offset, copy.size);
        if (dst) dst->MarkUsage(copy.dst_offset, copy.size);
        if (src) src->MarkUsage(copy.src_offset, copy.size);
    }
}
void BufferCacheRuntime::CopyBuffer(Buffer& dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies, bool, bool) { Copy(&dst, dst.Handle(), &src, src.Handle(), copies); }
void BufferCacheRuntime::CopyBuffer(Buffer& dst, ID3D12Resource* src, std::span<const VideoCommon::BufferCopy> copies, bool, bool) { Copy(&dst, dst.Handle(), nullptr, src, copies); }
void BufferCacheRuntime::CopyBuffer(ID3D12Resource* dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies, bool, bool) { Copy(nullptr, dst, &src, src.Handle(), copies); }

void BufferCacheRuntime::ClearBuffer(Buffer& dst, u32 offset, size_t size, u32 value) {
    auto upload = staging.Request(size, MemoryUsage::Upload);
    for (size_t pos = 0; pos < size; pos += sizeof(value))
        std::memcpy(upload.mapped_span.data() + pos, &value, std::min(sizeof(value), size - pos));
    const VideoCommon::BufferCopy copy{.src_offset = upload.offset, .dst_offset = offset, .size = size};
    CopyBuffer(dst, upload.buffer, {&copy, 1}, true);
}

bool BufferCacheRuntime::IsEmulatedTopology(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::Quads:
    case PrimitiveTopology::QuadStrip:
    case PrimitiveTopology::TriangleFan:
    case PrimitiveTopology::Polygon:
    case PrimitiveTopology::LineLoop:
        return true;
    default:
        return false;
    }
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat format,
                                         u32 first, u32 num_indices, Buffer& buffer, u32 offset,
                                         u32 size) {
    rewritten_count.reset();
    if (trace_buffers) {
        const bool rewritten = IsEmulatedTopology(topology) || format == IndexFormat::UnsignedByte;
        trace_buffers->push_back({.kind = rewritten ? TracedBuffer::Kind::RewrittenIndex
                                                    : TracedBuffer::Kind::Index,
                                  .buffer = &buffer, .offset = offset, .size = size,
                                  .stride = IndexSize(format),
                                  .device_addr = buffer.CpuAddr() + offset});
    }
    if (!IsEmulatedTopology(topology) && format != IndexFormat::UnsignedByte) {
        buffer.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
        pending_index = D3D12_INDEX_BUFFER_VIEW{
            .BufferLocation = buffer.Address() + offset,
            .SizeInBytes = size,
            .Format = format == IndexFormat::UnsignedShort ? DXGI_FORMAT_R16_UINT
                                                           : DXGI_FORMAT_R32_UINT,
        };
        return;
    }
    // D3D12 has neither these topologies nor 8-bit indices: rewrite the guest's indices on the
    // CPU from guest memory (the Vulkan backend uses compute passes instead).
    // TODO: inline index draws (inline_index_draw_indexes) never reach guest memory.
    if (!logged_rewrite) {
        LOG_INFO(Render, "D3D12: rewriting indexed draws on the CPU (topology {}, format {})",
                 static_cast<u32>(topology), static_cast<u32>(format));
        logged_rewrite = true;
    }
    const u32 index_size = IndexSize(format);
    guest_indices.resize(static_cast<size_t>(num_indices) * index_size);
    device_memory.ReadBlockUnsafe(buffer.CpuAddr() + offset + static_cast<u64>(first) * index_size,
                                  guest_indices.data(), guest_indices.size());
    const auto read = [&](u32 i) -> u32 {
        const u8* const data = guest_indices.data() + static_cast<size_t>(i) * index_size;
        switch (index_size) {
        case 1:
            return data[0];
        case 2: {
            u16 value;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }
        default: {
            u32 value;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }
        }
    };
    AssembleTopology(topology, num_indices, read, rewrite_scratch);
    BindRewrittenIndices(rewrite_scratch, format == IndexFormat::UnsignedInt);
}

void BufferCacheRuntime::EmulateTopology(PrimitiveTopology topology, u32 first, u32 count) {
    AssembleTopology(topology, count, [first](u32 i) { return first + i; }, rewrite_scratch);
    BindRewrittenIndices(rewrite_scratch, first + count > 0xFFFF);
}

void BufferCacheRuntime::BindRewrittenIndices(std::span<const u32> indices, bool wide) {
    rewritten_count = static_cast<u32>(indices.size());
    if (indices.empty()) {
        return;
    }
    const size_t index_size = wide ? sizeof(u32) : sizeof(u16);
    const size_t bytes = indices.size() * index_size;
    const StagingBufferRef upload = staging.Request(bytes, MemoryUsage::Upload);
    if (wide) {
        std::memcpy(upload.mapped_span.data(), indices.data(), bytes);
    } else {
        u8* out = upload.mapped_span.data();
        for (const u32 index : indices) {
            const u16 narrow = static_cast<u16>(index);
            std::memcpy(out, &narrow, sizeof(narrow));
            out += sizeof(narrow);
        }
    }
    pending_index = D3D12_INDEX_BUFFER_VIEW{
        .BufferLocation = upload.buffer->GetGPUVirtualAddress() + upload.offset,
        .SizeInBytes = static_cast<UINT>(bytes),
        .Format = wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
    };
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size,
                                          u32 stride) {
    if (trace_buffers) {
        trace_buffers->push_back({.kind = TracedBuffer::Kind::Vertex, .slot = index,
                                  .buffer = &buffer, .offset = offset, .size = size,
                                  .stride = stride, .device_addr = buffer.CpuAddr() + offset});
    }
    buffer.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    BindVertexBuffer(index, buffer.Handle(), offset, size, stride);
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, ID3D12Resource* buffer, u32 offset,
                                          u32 size, u32 stride) {
    if (index >= VideoCommon::NUM_VERTEX_BUFFERS || !buffer) {
        return;
    }
    pending_vertex[index] = D3D12_VERTEX_BUFFER_VIEW{
        .BufferLocation = buffer->GetGPUVirtualAddress() + offset,
        .SizeInBytes = size,
        .StrideInBytes = stride,
    };
    pending_vertex_mask |= 1u << index;
}

void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    for (u32 i = bindings.min_index; i < bindings.max_index; ++i) {
        const u32 slot = i - bindings.min_index;
        BindVertexBuffer(i, *bindings.buffers[slot], static_cast<u32>(bindings.offsets[slot]),
                         static_cast<u32>(bindings.sizes[slot]),
                         static_cast<u32>(bindings.strides[slot]));
    }
}

void BufferCacheRuntime::ApplyGeometry(ID3D12GraphicsCommandList* cmd) {
    if (pending_index) {
        cmd->IASetIndexBuffer(&*pending_index);
    }
    for (u32 mask = pending_vertex_mask; mask != 0; mask &= mask - 1) {
        const u32 index = static_cast<u32>(std::countr_zero(mask));
        cmd->IASetVertexBuffers(index, 1, &pending_vertex[index]);
    }
    pending_index.reset();
    pending_vertex_mask = 0;
    rewritten_count.reset();
}

std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(size_t, u32, u32 size) {
    VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::CbvStreamed, 1);
    const StagingBufferRef ref =
        staging.Request(Common::AlignUp(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT),
                        MemoryUsage::Upload);
    if (trace_buffers) {
        trace_buffers->push_back({.kind = TracedBuffer::Kind::StreamedUniform,
                                  .slot = traced_uniforms++, .size = size,
                                  .mapped = ref.mapped_span.data()});
    }
    if (descriptor_queue) {
        descriptor_queue->AddConstantBuffer(ref.buffer->GetGPUVirtualAddress() + ref.offset,
                                            size);
    }
    return ref.mapped_span.first(size);
}

void BufferCacheRuntime::BindUniformBuffer(Buffer& buffer, u32 offset, u32 size) {
    if (!descriptor_queue) {
        return;
    }
    // A CBV spans whole 256-byte blocks and must stay inside its resource; the null buffer (an
    // unbound guest cbuf) does not hold one.
    const u64 aligned = Common::AlignUp(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const bool fits = buffer.Handle() && buffer.SizeBytes() != 0 &&
                      offset + aligned <= Common::AlignUp(buffer.SizeBytes(),
                                                          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (trace_buffers) {
        trace_buffers->push_back({.kind = fits ? TracedBuffer::Kind::Uniform
                                               : TracedBuffer::Kind::NullUniform,
                                  .slot = traced_uniforms++, .buffer = fits ? &buffer : nullptr,
                                  .offset = offset, .size = size,
                                  .stride = static_cast<u32>(buffer.SizeBytes()),
                                  .device_addr = buffer.CpuAddr() + offset});
    }
    if (!fits) {
        VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::CbvNull, 1);
        descriptor_queue->AddConstantBuffer(0, 0);
        return;
    }
    VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::CbvPersistent, 1);
    buffer.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    descriptor_queue->AddConstantBuffer(buffer.Address() + offset, size);
}

void BufferCacheRuntime::BindStorageBuffer(Buffer& buffer, u32 offset, u32 size,
                                           bool is_written) {
    if (!descriptor_queue) {
        return;
    }
    // A buffer the cache merged with other data (vertices, say) stays in the state of its last
    // binding; AMD hardware, the Series included, reads buffers in any state.
    buffer.Transition(is_written ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                 : D3D12_RESOURCE_STATE_GENERIC_READ);
    const u64 available = buffer.SizeBytes() > offset ? buffer.SizeBytes() - offset : 0;
    const u32 clamped = static_cast<u32>(std::min<u64>(size, available));
    descriptor_queue->AddStorageBuffer(clamped != 0 ? buffer.Handle() : nullptr, offset, clamped);
}

void BufferCacheRuntime::BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                                           VideoCore::Surface::PixelFormat format) {
    if (!descriptor_queue) {
        return;
    }
    buffer.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    descriptor_queue->AddTexelBuffer(buffer.Handle(), offset, size, SurfaceFormat(format).view,
                                     VideoCore::Surface::BytesPerBlock(format), false);
}

void BufferCacheRuntime::BindImageBuffer(Buffer& buffer, u32 offset, u32 size,
                                         VideoCore::Surface::PixelFormat format) {
    if (!descriptor_queue) {
        return;
    }
    buffer.Transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    descriptor_queue->AddTexelBuffer(buffer.Handle(), offset, size, SurfaceFormat(format).view,
                                     VideoCore::Surface::BytesPerBlock(format), true);
}

void BufferCacheRuntime::RunSelfTest() {
    constexpr size_t test_size = 4096;
    Buffer gpu_buffer{*this, VAddr{0}, test_size};
    auto upload = UploadStagingBuffer(test_size);
    auto readback = DownloadStagingBuffer(test_size, true);
    for (size_t i = 0; i < test_size; ++i) upload.mapped_span[i] = static_cast<u8>(i * 37 + 11);
    const VideoCommon::BufferCopy up{.src_offset = upload.offset, .dst_offset = 0, .size = test_size};
    CopyBuffer(gpu_buffer, upload.buffer, {&up, 1}, true);
    const VideoCommon::BufferCopy down{.src_offset = 0, .dst_offset = 0, .size = test_size};
    CopyBuffer(readback.buffer, gpu_buffer, {&down, 1}, true);
    FreeDeferredStagingBuffer(readback);
    Finish();
    for (size_t i = 0; i < test_size; ++i) {
        if (readback.mapped_span[i] != static_cast<u8>(i * 37 + 11))
            throw std::runtime_error("D3D12: buffer cache round-trip self-test mismatch");
    }
    LOG_INFO(Render, "D3D12: buffer cache round-trip passed ({} bytes)", test_size);
}

template class VideoCommon::BufferCache<BufferCacheParams>;

} // namespace D3D12
