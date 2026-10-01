// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <climits>
#include <optional>
#include <span>
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

    /// Frame boundary only, with both texture and buffer cache mutexes held.
    /// Reclaims retired resources; deferred readbacks remain pinned until their owner releases them.
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
    StagingBuffersCache& GetCache(MemoryUsage usage);
    void ReleaseLevel(StagingBuffersCache& cache, size_t log2);

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
    MemoryGuardPolicy memory_guard;
    u64 stream_target = 256ULL * 1024 * 1024;
    bool stream_retiring{};
    bool emergency_finished{};
    unsigned guard_trim_cursor{};
    unsigned ring_retry_frames{};

    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    size_t current_delete_level = 0;
    u64 unique_ids{};
    u64 pending_tick{};
    u64 pending_upload_bytes{};
};

/// Creates a buffer in heap_type (UPLOAD or READBACK) and maps it for its whole lifetime.
ComPtr<ID3D12Resource> CreateMappedBuffer(ID3D12Device* device, u64 size,
                                          D3D12_HEAP_TYPE heap_type, std::span<u8>& mapped);

} // namespace D3D12
