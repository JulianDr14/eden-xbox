// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "video_core/query_cache.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class RasterizerD3D12;
class Scheduler;
class CachedQuery;
class HostCounter;
class QueryCache;

using CounterStream = VideoCommon::CounterStreamBase<QueryCache, HostCounter>;

/// Query heap slots, each with its own place in a readback buffer, recycled once the GPU is past
/// their last use (as Vulkan's QueryPool). The cache starts three counters on every command list,
/// so creating a heap and a readback buffer per counter stalled every submit.
class QueryPool {
public:
    struct Slot {
        ID3D12QueryHeap* heap{};
        ID3D12Resource* readback{};
        u32 index{};
        u64 offset{}; ///< byte offset of the resolved value in readback
    };

    QueryPool(const Device& device, Scheduler& scheduler);

    [[nodiscard]] Slot Reserve(VideoCore::QueryType type);
    /// Returns slot to the pool; it is handed out again once the GPU has completed tick.
    void Release(VideoCore::QueryType type, const Slot& slot, u64 tick);

private:
    static constexpr u32 PAGE_SIZE = 256;

    struct Page {
        ComPtr<ID3D12QueryHeap> heap;
        ComPtr<ID3D12Resource> readback;
    };
    struct FreeSlot {
        Slot slot;
        u64 tick;
    };
    struct TypePool {
        std::vector<Page> pages;
        std::deque<FreeSlot> free; ///< in release order, so roughly in tick order
    };

    void AddPage(VideoCore::QueryType type);

    const Device& device;
    Scheduler& scheduler;
    std::mutex mutex;
    std::array<TypePool, VideoCore::NumQueryTypes> pools;
};

/// A guest query report as a value known on the CPU plus the host query slices still counting
/// into it, so its value can be compared on the GPU instead of read back (ConditionalRendering).
struct PendingReport {
    static constexpr size_t MAX_SLICES = 4;

    u64 known{};
    std::array<std::shared_ptr<HostCounter>, MAX_SLICES> slices{}; ///< newest first
    size_t num_slices{};
    bool complete{true}; ///< false when more than MAX_SLICES slices count into it
};

class QueryCache final
    : public VideoCommon::QueryCacheLegacy<QueryCache, CachedQuery, CounterStream, HostCounter> {
public:
    QueryCache(RasterizerD3D12& rasterizer, Tegra::MaxwellDeviceMemoryManager& device_memory,
               const Device& device, Scheduler& scheduler);
    ~QueryCache();

    [[nodiscard]] const Device& GetDevice() const noexcept { return device; }
    [[nodiscard]] Scheduler& GetScheduler() const noexcept { return scheduler; }
    [[nodiscard]] const std::shared_ptr<QueryPool>& GetPool() const noexcept { return pool; }
    [[nodiscard]] bool AnyCommandQueued() const noexcept;

    /// The report the guest reads at addr, without waiting for the GPU; nullopt when no query is
    /// cached there (the value is in guest memory).
    [[nodiscard]] std::optional<PendingReport> PeekReport(VAddr addr);

private:
    RasterizerD3D12& rasterizer;
    const Device& device;
    Scheduler& scheduler;
    /// Shared with the counters: the base class's streams, and the counters they hold, are
    /// destroyed after this class's members.
    std::shared_ptr<QueryPool> pool;
};

class HostCounter final : public VideoCommon::HostCounterBase<QueryCache, HostCounter> {
public:
    HostCounter(QueryCache& cache, std::shared_ptr<HostCounter> dependency,
                VideoCore::QueryType type);
    ~HostCounter();
    void EndQuery();

    [[nodiscard]] bool Ended() const noexcept { return ended; }
    [[nodiscard]] const QueryPool::Slot& Slot() const noexcept { return slot; }
    [[nodiscard]] D3D12_QUERY_TYPE HostType() const noexcept { return query_type; }
    /// Bytes ResolveQueryData writes for this query, and where the counted value is in them.
    [[nodiscard]] u64 ResolvedBytes() const noexcept;
    [[nodiscard]] u64 ValueOffset() const noexcept;

private:
    u64 BlockingQuery(bool async = false) const override;

    QueryCache& cache;
    VideoCore::QueryType type;
    D3D12_QUERY_TYPE query_type{};
    std::shared_ptr<QueryPool> pool;
    QueryPool::Slot slot;
    mutable u64 tick{};
    bool ended{};
};

class CachedQuery final : public VideoCommon::CachedQueryBase<HostCounter> {
public:
    CachedQuery(QueryCache& cache, VideoCore::QueryType type, VAddr cpu_addr, u8* host_ptr);
    ~CachedQuery() override = default;
    CachedQuery(CachedQuery&&) noexcept = default;
    CachedQuery& operator=(CachedQuery&&) noexcept = default;
    CachedQuery(const CachedQuery&) = delete;
    CachedQuery& operator=(const CachedQuery&) = delete;
};

} // namespace D3D12
