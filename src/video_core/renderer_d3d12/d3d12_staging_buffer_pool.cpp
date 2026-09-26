// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/literals.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {

namespace {

using namespace Common::Literals;

constexpr u64 STREAM_BUFFER_SIZE = 128_MiB;

} // Anonymous namespace

ComPtr<ID3D12Resource> CreateMappedBuffer(ID3D12Device* device, u64 size,
                                          D3D12_HEAP_TYPE heap_type, std::span<u8>& mapped) {
    const D3D12_HEAP_PROPERTIES heap{.Type = heap_type};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    // These are the only states the two heap types allow, for their whole lifetime.
    const D3D12_RESOURCE_STATES state = heap_type == D3D12_HEAP_TYPE_READBACK
                                            ? D3D12_RESOURCE_STATE_COPY_DEST
                                            : D3D12_RESOURCE_STATE_GENERIC_READ;
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                  nullptr, IID_PPV_ARGS(&buffer)),
                  "CreateCommittedResource (staging)");
    void* pointer{};
    ThrowIfFailed(buffer->Map(0, nullptr, &pointer), "ID3D12Resource::Map");
    mapped = std::span(static_cast<u8*>(pointer), static_cast<size_t>(size));
    return buffer;
}

StagingBufferPool::StagingBufferPool(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_}, stream_buffer_size{STREAM_BUFFER_SIZE},
      region_size{STREAM_BUFFER_SIZE / NUM_SYNCS} {
    stream_buffer = CreateMappedBuffer(device.Get(), stream_buffer_size, D3D12_HEAP_TYPE_UPLOAD,
                                       stream_pointer);
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred) {
    if (!deferred && usage == MemoryUsage::Upload && size <= region_size) {
        return GetStreamBuffer(size);
    }
    return GetStagingBuffer(size, usage, deferred);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    auto& entries = GetCache(ref.usage)[ref.log2_level].entries;
    const auto it = std::find_if(entries.begin(), entries.end(), [&ref](const StagingBuffer& e) {
        return e.index == ref.index;
    });
    ASSERT(it != entries.end());
    ASSERT(it->deferred);
    it->tick = scheduler.CurrentTick();
    it->deferred = false;
}

void StagingBufferPool::TickFrame() {
    current_delete_level = (current_delete_level + 1) % NUM_LEVELS;
    ReleaseLevel(upload_cache, current_delete_level);
    ReleaseLevel(download_cache, current_delete_level);
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(size_t size) {
    if (AreRegionsActive(Region(free_iterator) + 1,
                         std::min(Region(iterator + size) + 1, NUM_SYNCS))) {
        // The GPU still reads the next regions: use a dedicated buffer instead of waiting.
        return GetStagingBuffer(size, MemoryUsage::Upload);
    }
    const u64 current_tick = scheduler.CurrentTick();
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;
    free_iterator = std::max(free_iterator, iterator + size);

    if (iterator + size >= stream_buffer_size) {
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + NUM_SYNCS,
                  current_tick);
        used_iterator = 0;
        iterator = 0;
        free_iterator = size;
        if (AreRegionsActive(0, Region(size) + 1)) {
            return GetStagingBuffer(size, MemoryUsage::Upload);
        }
    }
    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, ALIGNMENT);
    return StagingBufferRef{
        .buffer = stream_buffer.Get(),
        .offset = offset,
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage = MemoryUsage::Upload,
        .log2_level{},
        .index{},
    };
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    const u64 gpu_tick = scheduler.KnownGpuTick();
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                       [gpu_tick](u64 sync_tick) { return gpu_tick < sync_tick; });
}

StagingBufferRef StagingBufferPool::GetStagingBuffer(size_t size, MemoryUsage usage,
                                                     bool deferred) {
    if (const std::optional<StagingBufferRef> ref = TryGetReservedBuffer(size, usage, deferred)) {
        return *ref;
    }
    return CreateStagingBuffer(size, usage, deferred);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(size_t size,
                                                                        MemoryUsage usage,
                                                                        bool deferred) {
    StagingBuffers& cache_level = GetCache(usage)[Common::Log2Ceil64(size)];
    const auto is_free = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };
    auto& entries = cache_level.entries;
    const auto hint_it = entries.begin() + cache_level.iterate_index;
    auto it = std::find_if(hint_it, entries.end(), is_free);
    if (it == entries.end()) {
        it = std::find_if(entries.begin(), hint_it, is_free);
        if (it == hint_it) {
            return std::nullopt;
        }
    }
    cache_level.iterate_index = std::distance(entries.begin(), it) + 1;
    it->tick = deferred ? std::numeric_limits<u64>::max() : scheduler.CurrentTick();
    it->deferred = deferred;
    return it->Ref();
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(size_t size, MemoryUsage usage,
                                                        bool deferred) {
    const u32 log2 = Common::Log2Ceil64(size);
    const D3D12_HEAP_TYPE heap_type =
        usage == MemoryUsage::Download ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
    std::span<u8> mapped;
    ComPtr<ID3D12Resource> buffer = CreateMappedBuffer(device.Get(), 1ULL << log2, heap_type,
                                                       mapped);
    StagingBuffer& entry = GetCache(usage)[log2].entries.emplace_back(StagingBuffer{
        .buffer = std::move(buffer),
        .mapped_span = mapped,
        .usage = usage,
        .log2_level = log2,
        .index = unique_ids++,
        .tick = deferred ? std::numeric_limits<u64>::max() : scheduler.CurrentTick(),
        .deferred = deferred,
    });
    return entry.Ref();
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    return usage == MemoryUsage::Download ? download_cache : upload_cache;
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2) {
    constexpr size_t deletions_per_tick = 16;
    auto& staging = cache[log2];
    auto& entries = staging.entries;
    const size_t begin_offset = staging.delete_index;
    const size_t end_offset = std::min(begin_offset + deletions_per_tick, entries.size());
    const auto begin = entries.begin() + begin_offset;
    const auto end = entries.begin() + end_offset;
    entries.erase(std::remove_if(begin, end,
                                 [this](const StagingBuffer& entry) {
                                     return !entry.deferred && scheduler.IsFree(entry.tick);
                                 }),
                  end);
    const size_t new_size = entries.size();
    staging.delete_index += deletions_per_tick;
    if (staging.delete_index >= new_size) {
        staging.delete_index = 0;
    }
    if (staging.iterate_index > new_size) {
        staging.iterate_index = 0;
    }
}

} // namespace D3D12
