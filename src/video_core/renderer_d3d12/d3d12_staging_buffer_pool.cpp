// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/literals.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {

namespace {

using namespace Common::Literals;

constexpr u64 STREAM_BUFFER_SIZE = 128_MiB;
/// Largest upload the stream serves; it may span several regions. Larger ones (and deferred ones)
/// get dedicated buffers. Up to 0.2.50 only one region (8 MiB) was served: a 2048x2048 texture
/// with mips decoded to RGBA8 (22 MB) then took a new 32 MiB buffer, and a loading screen in
/// Mario Wonder created a dozen of them in three seconds on top of a nearly full memory budget.
constexpr u64 MAX_STREAM_REQUEST = STREAM_BUFFER_SIZE / 4;
/// Uploads at least this large wait for the GPU to release submitted regions instead of
/// allocating a dedicated buffer; smaller ones never stall the CPU.
constexpr u64 WAIT_FOR_STREAM_SIZE = 1_MiB;

} // Anonymous namespace

ComPtr<ID3D12Resource> CreateMappedBuffer(ID3D12Device* device, u64 size,
                                          D3D12_HEAP_TYPE heap_type, std::span<u8>& mapped) {
    if (size == 0) {
        throw std::invalid_argument("D3D12: staging buffer size must not be zero");
    }
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
    VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::ResourceCreateUs,
                                       VideoCore::Perf::Counter::ResourcesCreated};
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                  nullptr, IID_PPV_ARGS(&buffer)),
                  "CreateCommittedResource (staging)");
    CheckRemovedAfter(device, [&] {
        return fmt::format("creating a {} byte staging buffer (heap {})", desc.Width,
                           static_cast<u32>(heap_type));
    });
    void* pointer{};
    const D3D12_RANGE no_cpu_reads{0, 0};
    const D3D12_RANGE* const read_range =
        heap_type == D3D12_HEAP_TYPE_UPLOAD ? &no_cpu_reads : nullptr;
    ThrowIfFailed(buffer->Map(0, read_range, &pointer), "ID3D12Resource::Map");
    mapped = std::span(static_cast<u8*>(pointer), static_cast<size_t>(size));
    return buffer;
}

StagingBufferPool::StagingBufferPool(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_}, stream_buffer_size{STREAM_BUFFER_SIZE},
      region_size{STREAM_BUFFER_SIZE / NUM_SYNCS} {
    stream_buffer = CreateMappedBuffer(device.Get(), stream_buffer_size, D3D12_HEAP_TYPE_UPLOAD,
                                       stream_pointer);
    constexpr u64 bytes_per_mib = 1024 * 1024;
    LOG_INFO(Render, "D3D12: staging stream ready ({} MiB, {} regions of {} MiB)",
             stream_buffer_size / bytes_per_mib, NUM_SYNCS, region_size / bytes_per_mib);
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred) {
    // The generic caches may ask for zero bytes (e.g. a download batch with no images); hand out a
    // minimal allocation rather than throwing on what may be Eden's fence thread.
    size = std::max<size_t>(size, 1);
    if (usage == MemoryUsage::Upload) {
        const u64 tick = scheduler.CurrentTick();
        if (pending_tick != tick) {
            pending_tick = tick;
            pending_upload_bytes = 0;
        }
        pending_upload_bytes += size;
    }
    if (!deferred && usage == MemoryUsage::Upload && size <= MAX_STREAM_REQUEST) {
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
    size_t offset = Common::AlignUp(iterator, ALIGNMENT);
    bool wrapped = false;
    if (offset > stream_buffer_size || size > stream_buffer_size - offset) {
        offset = 0;
        wrapped = true;
    }

    const size_t first_region = Region(offset);
    const size_t end_region = Region(offset + size - 1) + 1;
    // Every region this allocation enters for the first time in this lap may still hold data the
    // GPU reads from the previous lap - not only after wrapping. The region the iterator is already
    // in was validated when this lap entered it, so it is skipped.
    size_t check_begin = first_region;
    if (!wrapped && iterator > 0 && Region(iterator - 1) == first_region) {
        ++check_begin;
    }
    if (check_begin < end_region && AreRegionsActive(check_begin, end_region)) {
        // Small uploads do not stall the CPU on memory still consumed by the GPU. Large ones wait
        // for it when that work is already submitted: a dedicated buffer per texture is what ran
        // the console out of memory. Regions of the list being recorded cannot be waited on here
        // (flushing mid-draw would lose its state); the rasterizer submits upload-heavy lists
        // between draws instead (RasterizerD3D12::FlushIfUploadHeavy).
        const u64 busy_tick = *std::max_element(sync_ticks.begin() + check_begin,
                                                sync_ticks.begin() + end_region);
        if (size < WAIT_FOR_STREAM_SIZE || busy_tick >= scheduler.CurrentTick() ||
            !scheduler.IsRecordingThread()) {
            return GetStagingBuffer(size, MemoryUsage::Upload);
        }
        scheduler.Wait(busy_tick);
        VideoCore::Perf::Add(VideoCore::Perf::Counter::StagingStreamWaits, 1);
    }

    std::fill(sync_ticks.begin() + first_region, sync_ticks.begin() + end_region,
              scheduler.CurrentTick());
    iterator = offset + size;

    if (!logged_stream_use) {
        LOG_INFO(Render, "D3D12: staging stream path active ({} bytes at offset {})", size,
                 offset);
        logged_stream_use = true;
    }
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
    // Strict, as in the Vulkan pool: a region stamped with the tick being recorded is busy too, or a
    // single command list that laps the ring would overwrite its own unsubmitted uploads. Regions
    // shared by consecutive allocations in one lap are skipped by the caller instead.
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
    const u32 log2 = static_cast<u32>(Common::Log2Ceil<u64>(size));
    if (log2 >= NUM_LEVELS) {
        throw std::length_error("D3D12: staging request is too large");
    }
    StagingBuffers& cache_level = GetCache(usage)[log2];
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
    const u32 log2 = static_cast<u32>(Common::Log2Ceil<u64>(size));
    if (log2 >= NUM_LEVELS) {
        throw std::length_error("D3D12: staging request is too large");
    }
    const D3D12_HEAP_TYPE heap_type =
        usage == MemoryUsage::Download ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
    std::span<u8> mapped;
    ComPtr<ID3D12Resource> buffer = CreateMappedBuffer(device.Get(), 1ULL << log2, heap_type,
                                                       mapped);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::StagingDedicated, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::StagingDedicatedBytes, 1ULL << log2);
    StagingBuffer& entry = GetCache(usage)[log2].entries.emplace_back(StagingBuffer{
        .buffer = std::move(buffer),
        .mapped_span = mapped,
        .usage = usage,
        .log2_level = log2,
        .index = unique_ids++,
        .tick = deferred ? std::numeric_limits<u64>::max() : scheduler.CurrentTick(),
        .deferred = deferred,
    });
    // Every one used to be logged; a loading screen creates thousands of small ones, and the log
    // itself then slowed the frame. Large ones and the first few still are.
    constexpr u64 LOGGED_SIZE = 1_MiB;
    constexpr u64 LOGGED_FIRST = 32;
    if ((1ULL << log2) >= LOGGED_SIZE || unique_ids <= LOGGED_FIRST) {
        LOG_INFO(Render, "D3D12: created dedicated staging {} buffer ({} bytes, request {} bytes)",
                 usage == MemoryUsage::Download ? "readback" : "upload", 1ULL << log2, size);
    }
    return entry.Ref();
}

u64 StagingBufferPool::PendingUploadBytes() const noexcept {
    return pending_tick == scheduler.CurrentTick() ? pending_upload_bytes : 0;
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
