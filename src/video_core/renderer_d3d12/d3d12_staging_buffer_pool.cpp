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
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {

namespace {

using namespace Common::Literals;

constexpr u64 STREAM_BUFFER_SIZE = 256_MiB;
/// Largest upload the stream serves; it may span several regions. Larger ones (and deferred ones)
/// get dedicated buffers. Up to 0.2.50 only one region (8 MiB) was served: a 2048x2048 texture
/// with mips decoded to RGBA8 (22 MB) then took a new 32 MiB buffer, and a loading screen in
/// Mario Wonder created a dozen of them in three seconds on top of a nearly full memory budget.
// Keep the request cutoff independent of ring capacity so capacity experiments do not also
// change which uploads use dedicated buffers.
constexpr u64 MAX_STREAM_REQUEST = 32_MiB;
/// Uploads at least this large wait for the GPU to release submitted regions instead of
/// allocating a dedicated buffer; smaller ones never stall the CPU.
constexpr u64 WAIT_FOR_STREAM_SIZE = 1_MiB;

} // Anonymous namespace

ComPtr<ID3D12Resource> CreateMappedBuffer(ID3D12Device* device, u64 size,
                                          D3D12_HEAP_TYPE heap_type, std::span<u8>& mapped) {
    if (size == 0) {
        throw std::invalid_argument("D3D12: staging buffer size must not be zero");
    }
    // These are the only states the two heap types allow, for their whole lifetime.
    const D3D12_RESOURCE_STATES state = heap_type == D3D12_HEAP_TYPE_READBACK
                                            ? D3D12_RESOURCE_STATE_COPY_DEST
                                            : D3D12_RESOURCE_STATE_GENERIC_READ;
    HRESULT hr{};
    ComPtr<ID3D12Resource> buffer =
        CreateCommittedBuffer(device, size, heap_type, state, D3D12_RESOURCE_FLAG_NONE,
                              "CreateCommittedResource (staging)", &hr);
    if (hr == E_OUTOFMEMORY) {
        throw StagingOutOfMemory(
            fmt::format("D3D12: out of memory creating a {} byte staging buffer", size));
    }
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
    if (!deferred && usage == MemoryUsage::Upload && size <= MAX_STREAM_REQUEST &&
        stream_buffer && !ring_guard.Retiring() && size <= stream_buffer_size) {
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

bool StagingBufferPool::GuardMemory(const CacheMemorySnapshot& snapshot) {
    headroom.Measured(snapshot.app_used, snapshot.app_limit);
    const auto plan = ring_guard.BeginFrame(snapshot.app_used, snapshot.app_limit,
                                            stream_buffer ? stream_buffer_size : 0);
    if (plan.target != plan.previous_target) {
        LOG_INFO(Render, "D3D12: memory guard headroom={} KiB, staging target {} -> {} MiB",
                 snapshot.AppFree() / 1024, plan.previous_target / 1_MiB, plan.target / 1_MiB);
    }
    // Only the last-resort branch waits; ordinary reclamation never flushes or waits for the GPU.
    // Retiring() already stops new ring references, including ones for unsubmitted work.
    if (plan.finish) {
        scheduler.Finish();
    }
    bool released_ring = false;
    if (ring_guard.Retiring() &&
        *std::max_element(sync_ticks.begin(), sync_ticks.end()) <= scheduler.KnownGpuTick()) {
        const u64 released = stream_buffer_size;
        stream_buffer.Reset();
        stream_pointer = {};
        stream_buffer_size = region_size = iterator = 0;
        sync_ticks.fill(0);
        ring_guard.OnRetired();
        released_ring = true;
        LOG_INFO(Render, "D3D12: memory guard retired staging ring ({} MiB released)",
                 released / 1_MiB);
    }
    // A ring released this frame is not in the snapshot yet, so its replacement waits a frame.
    if (const u64 wanted = ring_guard.WantedRing(stream_buffer ? stream_buffer_size : 0,
                                                 snapshot.app_used, snapshot.app_limit)) {
        try {
            stream_buffer = CreateMappedBuffer(device.Get(), wanted, D3D12_HEAP_TYPE_UPLOAD,
                                               stream_pointer);
            stream_buffer_size = wanted;
            region_size = wanted / NUM_SYNCS;
            logged_stream_use = false;
            headroom.Created(wanted);
            LOG_INFO(Render, "D3D12: memory guard staging ring at {} MiB", wanted / 1_MiB);
        } catch (const std::exception& e) {
            // Dedicated uploads remain available; back off instead of retrying every frame.
            ring_guard.OnAllocationFailed();
            LOG_WARNING(Render, "D3D12: memory guard ring allocation deferred: {}", e.what());
        }
    }
    if (plan.trim) {
        // Bounded sweep: at most 16 entries in each of four buckets per frame (every bucket in an
        // emergency). ReleaseLevel checks fence completion and pinned ownership, allocating nothing.
        for (unsigned i = 0; i < (plan.emergency ? NUM_LEVELS : 4); ++i) {
            const auto level = NUM_LEVELS - 1 - guard_trim_cursor++ % NUM_LEVELS;
            ReleaseLevel(upload_cache, level);
            ReleaseLevel(download_cache, level);
        }
    }
    return released_ring;
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
        .capacity = size,
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
    // A bucket holds up to sixteen sizes (DedicatedStagingSize): reuse only one large enough.
    const auto is_free = [this, size](const StagingBuffer& entry) {
        return !entry.deferred && entry.mapped_span.size() >= size && scheduler.IsFree(entry.tick);
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
    const u64 bytes = DedicatedStagingSize(size);
    if (headroom.NeedsMeasure(bytes)) {
        MeasureHeadroom();
    }
    if (!headroom.Fits(bytes)) {
        if (const auto ref = RelieveMemoryPressure(size, usage, deferred, false)) {
            return *ref;
        }
    }
    const D3D12_HEAP_TYPE heap_type =
        usage == MemoryUsage::Download ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
    std::span<u8> mapped;
    ComPtr<ID3D12Resource> buffer;
    try {
        buffer = CreateMappedBuffer(device.Get(), bytes, heap_type, mapped);
    } catch (const StagingOutOfMemory& e) {
        // The commit limit refused it. Crossing the limit is what the platform punishes, so give
        // back what submitted work no longer needs and try exactly once more.
        LOG_WARNING(Render, "D3D12: {}; reclaiming and retrying", e.what());
        if (const auto ref = RelieveMemoryPressure(size, usage, deferred, true)) {
            return *ref;
        }
        buffer = CreateMappedBuffer(device.Get(), bytes, heap_type, mapped);
    }
    headroom.Created(bytes);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::StagingDedicated, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::StagingDedicatedBytes, bytes);
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
    if (bytes >= LOGGED_SIZE || unique_ids <= LOGGED_FIRST) {
        LOG_INFO(Render, "D3D12: created dedicated staging {} buffer ({} bytes, request {} bytes)",
                 usage == MemoryUsage::Download ? "readback" : "upload", bytes, size);
    }
    return entry.Ref();
}

std::optional<StagingBufferRef> StagingBufferPool::RelieveMemoryPressure(size_t size,
                                                                         MemoryUsage usage,
                                                                         bool deferred,
                                                                         bool allocation_failed) {
    // Only this usage's cache: the other may belong to a thread holding the other cache's mutex.
    StagingBuffersCache& cache = GetCache(usage);
    ReleaseAllRetired(cache);
    // Wait only on submitted work, so a draw being recorded is never split, and proactively once
    // per tick: under sustained pressure each command list stalls at most once, not per allocation.
    // A refused allocation always reclaims (a repeated wait on a passed tick returns at once).
    const u64 current = scheduler.CurrentTick();
    const u64 submitted = current > 0 ? current - 1 : 0;
    if (scheduler.IsRecordingThread() && (allocation_failed || submitted > pressure_wait_tick)) {
        pressure_wait_tick = submitted;
        if (allocation_failed || !scheduler.IsFree(submitted)) {
            scheduler.Wait(submitted);
            VideoCore::Perf::Add(VideoCore::Perf::Counter::StagingStreamWaits, 1);
        }
        // Deferred releases (textures, buffers) return their memory once their tick has passed.
        scheduler.CollectGarbage();
        if (auto ref = TryGetReservedBuffer(size, usage, deferred)) {
            return ref;
        }
        ReleaseAllRetired(cache);
    }
    MeasureHeadroom();
    return std::nullopt;
}

void StagingBufferPool::MeasureHeadroom() {
    const CacheMemorySnapshot snapshot = device.QueryCacheMemoryPressure();
    headroom.Measured(snapshot.app_used, snapshot.app_limit);
}

u64 StagingBufferPool::PendingUploadBytes() const noexcept {
    return pending_tick == scheduler.CurrentTick() ? pending_upload_bytes : 0;
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    return usage == MemoryUsage::Download ? download_cache : upload_cache;
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2,
                                     size_t max_checks) {
    auto& staging = cache[log2];
    if (staging.entries.empty()) {
        return;
    }
    const u64 completed = scheduler.KnownGpuTick();
    if (ReclaimRetiredStaging(staging.entries, staging.delete_index,
                             [completed](const StagingBuffer& entry) {
                                 return !entry.deferred && entry.tick <= completed;
                             },
                             max_checks)) {
        staging.iterate_index = 0;
    }
}

void StagingBufferPool::ReleaseAllRetired(StagingBuffersCache& cache) {
    for (size_t level = 0; level < NUM_LEVELS; ++level) {
        cache[level].delete_index = 0;
        ReleaseLevel(cache, level, std::numeric_limits<size_t>::max());
    }
}

} // namespace D3D12
