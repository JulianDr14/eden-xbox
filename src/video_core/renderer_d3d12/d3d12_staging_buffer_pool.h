// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <climits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_memory_guard.h"

namespace D3D12 {

class Scheduler;

enum class MemoryUsage {
    Upload,   ///< CPU writes, GPU reads (UPLOAD heap, always GENERIC_READ)
    Download, ///< GPU writes, CPU reads (READBACK heap, always COPY_DEST)
};

struct StagingBufferRef {
    ID3D12Resource* buffer;
    u64 offset;
    std::span<u8> mapped_span;
    MemoryUsage usage;
    u32 log2_level;
    u64 index;
    u64 capacity; ///< Bytes the buffer was created with (dedicated buffers); what it commits.
};

/// CPU-visible memory for uploads and downloads, a port of the Vulkan backend's pool:
///  - small uploads come from a persistently mapped stream ring split in NUM_SYNCS regions,
///    each retired by the tick that last used it;
///  - larger or deferred requests get dedicated buffers, bucketed by power-of-two size and reused
///    once their tick is done.
/// UPLOAD and READBACK resources can never change state, so none of this needs barriers.
class StagingBufferPool {
public:
    static constexpr size_t NUM_SYNCS = 16;
    /// Covers buffer->texture copies (512) as well as constant buffer views (256).
    static constexpr u64 ALIGNMENT = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

    explicit StagingBufferPool(const Device& device, Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    StagingBufferRef Request(size_t size, MemoryUsage usage, bool deferred = false);
    void FreeDeferred(StagingBufferRef& ref);

    [[nodiscard]] ID3D12Resource* StreamBuffer() const noexcept {
        return stream_buffer.Get();
    }

    void TickFrame();

    /// Frame boundary only, with both texture and buffer cache mutexes held. Resizes the stream
    /// ring to the app's headroom and reclaims retired resources; deferred readbacks remain pinned
    /// until their owner releases them. Returns whether the ring was released.
    /// The emergency branch drains the GPU with those mutexes held: safe because nothing the drain
    /// runs (Scheduler::CollectGarbage retire callbacks) acquires a cache mutex. Keep it that way.
    [[nodiscard]] bool GuardMemory(const CacheMemorySnapshot& snapshot);

    /// Upload bytes requested for the command list being recorded (stream and dedicated).
    [[nodiscard]] u64 PendingUploadBytes() const noexcept;

private:
    struct StagingBuffer {
        ComPtr<ID3D12Resource> buffer;
        std::span<u8> mapped_span;
        MemoryUsage usage;
        u32 log2_level;
        u64 index;
        u64 tick = 0;
        bool deferred{};

        StagingBufferRef Ref() const noexcept {
            return {
                .buffer = buffer.Get(),
                .offset = 0,
                .mapped_span = mapped_span,
                .usage = usage,
                .log2_level = log2_level,
                .index = index,
                .capacity = mapped_span.size(),
            };
        }
    };

    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        size_t delete_index = 0;
        size_t iterate_index = 0;
    };

    static constexpr size_t NUM_LEVELS = sizeof(size_t) * CHAR_BIT;
    using StagingBuffersCache = std::array<StagingBuffers, NUM_LEVELS>;

    StagingBufferRef GetStreamBuffer(size_t size);
    bool AreRegionsActive(size_t region_begin, size_t region_end) const;
    StagingBufferRef GetStagingBuffer(size_t size, MemoryUsage usage, bool deferred = false);
    std::optional<StagingBufferRef> TryGetReservedBuffer(size_t size, MemoryUsage usage,
                                                         bool deferred);
    StagingBufferRef CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred);
    /// Called before a dedicated buffer would cross HeadroomTracker::RESERVE, or after the OS
    /// refused one. Frees every retired buffer of this kind and, on the recording thread, waits for
    /// already-submitted work (at most once per tick; the list being recorded is never flushed) so
    /// a busy buffer can be reused instead of growing. Returns a reusable buffer if one freed up.
    std::optional<StagingBufferRef> RelieveMemoryPressure(size_t size, MemoryUsage usage,
                                                          bool deferred, bool allocation_failed);
    StagingBuffersCache& GetCache(MemoryUsage usage);
    void ReleaseLevel(StagingBuffersCache& cache, size_t log2, size_t max_checks = 16);
    void ReleaseAllRetired(StagingBuffersCache& cache);
    void MeasureHeadroom();

    size_t Region(size_t iter) const noexcept {
        return iter / region_size;
    }

    const Device& device;
    Scheduler& scheduler;

    ComPtr<ID3D12Resource> stream_buffer;
    std::span<u8> stream_pointer;
    u64 stream_buffer_size;
    u64 region_size;

    size_t iterator = 0;
    std::array<u64, NUM_SYNCS> sync_ticks{};

    bool logged_stream_use = false;
    StagingRingController ring_guard;
    HeadroomTracker headroom;
    u64 pressure_wait_tick{};
    unsigned guard_trim_cursor{};

    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    size_t current_delete_level = 0;
    u64 unique_ids{};
    u64 pending_tick{};
    u64 pending_upload_bytes{};
};

/// The OS refused the commit (E_OUTOFMEMORY): recoverable by releasing memory and retrying.
struct StagingOutOfMemory : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Creates a buffer in heap_type (UPLOAD or READBACK) and maps it for its whole lifetime.
/// Throws StagingOutOfMemory when the app's commit limit refuses it.
ComPtr<ID3D12Resource> CreateMappedBuffer(ID3D12Device* device, u64 size,
                                          D3D12_HEAP_TYPE heap_type, std::span<u8>& mapped);

} // namespace D3D12
