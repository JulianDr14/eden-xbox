// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <stdexcept>

#include "common/logging.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams params)
    : VideoCommon::BufferBase(params), buffer{runtime.CreateDefaultBuffer(4)}, tracker{4096} {}

Buffer::Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr, u64 size_bytes)
    : VideoCommon::BufferBase(cpu_addr, size_bytes),
      buffer{runtime.CreateDefaultBuffer(size_bytes)}, tracker{size_bytes} {}

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                       StagingBufferPool& staging_)
    : device{device_}, scheduler{scheduler_}, staging{staging_} {
    LOG_INFO(Render, "D3D12: buffer cache runtime ready");
}

ComPtr<ID3D12Resource> BufferCacheRuntime::CreateDefaultBuffer(u64 size) {
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                   .Width = size, .Height = 1, .DepthOrArraySize = 1,
                                   .MipLevels = 1, .Format = DXGI_FORMAT_UNKNOWN,
                                   .SampleDesc = {.Count = 1},
                                   .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> result;
    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result)),
                  "CreateCommittedResource (buffer cache)");
    return result;
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& buffers) noexcept {
    for (auto it = buffers.begin(); it != buffers.end(); ++it) {
        it->ResetUsageTracking();
    }
}
void BufferCacheRuntime::Finish() { scheduler.Finish(); }
u64 BufferCacheRuntime::GetDeviceLocalMemory() const { return 4ULL * 1024 * 1024 * 1024; }
u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    ComPtr<IDXGIAdapter3> adapter;
    if (FAILED(device.Factory()->EnumAdapters1(0, reinterpret_cast<IDXGIAdapter1**>(adapter.GetAddressOf())))) return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    return SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) ? info.CurrentUsage : 0;
}
StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) { return staging.Request(size, MemoryUsage::Upload); }
StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) { return staging.Request(size, MemoryUsage::Download, deferred); }
void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) { staging.FreeDeferred(ref); }

void BufferCacheRuntime::Copy(Buffer* dst, ID3D12Resource* dst_raw, Buffer* src,
                              ID3D12Resource* src_raw,
                              std::span<const VideoCommon::BufferCopy> copies) {
    auto* cmd = scheduler.CommandList();
    for (const auto& copy : copies) {
        cmd->CopyBufferRegion(dst_raw, copy.dst_offset, src_raw, copy.src_offset, copy.size);
        if (dst) dst->MarkUsage(copy.dst_offset, copy.size);
        if (src) src->MarkUsage(copy.src_offset, copy.size);
    }
    D3D12_RESOURCE_BARRIER barriers[2]{};
    u32 count = 0;
    const auto add_decay = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before) {
        barriers[count++] = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                             .Transition = {.pResource = resource,
                                            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                            .StateBefore = before,
                                            .StateAfter = D3D12_RESOURCE_STATE_COMMON}};
    };
    if (src) add_decay(src_raw, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (dst) add_decay(dst_raw, D3D12_RESOURCE_STATE_COPY_DEST);
    if (count) cmd->ResourceBarrier(count, barriers);
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

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology, IndexFormat format, u32, u32,
                                         Buffer& buffer, u32 offset, u32 size) {
    const DXGI_FORMAT dxgi = format == IndexFormat::UnsignedShort ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    const D3D12_INDEX_BUFFER_VIEW view{.BufferLocation = buffer.Address() + offset,
                                       .SizeInBytes = size, .Format = dxgi};
    scheduler.CommandList()->IASetIndexBuffer(&view);
}
void BufferCacheRuntime::BindVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size, u32 stride) {
    BindVertexBuffer(index, buffer.Handle(), offset, size, stride);
}
void BufferCacheRuntime::BindVertexBuffer(u32 index, ID3D12Resource* buffer, u32 offset, u32 size, u32 stride) {
    const D3D12_VERTEX_BUFFER_VIEW view{.BufferLocation = buffer->GetGPUVirtualAddress() + offset,
                                        .SizeInBytes = size, .StrideInBytes = stride};
    scheduler.CommandList()->IASetVertexBuffers(index, 1, &view);
}
void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    for (u32 i = bindings.min_index; i < bindings.max_index; ++i)
        BindVertexBuffer(i, *bindings.buffers[i], static_cast<u32>(bindings.offsets[i]),
                         static_cast<u32>(bindings.sizes[i]), static_cast<u32>(bindings.strides[i]));
}
std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(size_t, u32, u32 size) {
    return staging.Request(size, MemoryUsage::Upload).mapped_span;
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
