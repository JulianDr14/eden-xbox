// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "video_core/query_cache.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class RasterizerD3D12;
class Scheduler;
class CachedQuery;
class HostCounter;
class QueryCache;

using CounterStream = VideoCommon::CounterStreamBase<QueryCache, HostCounter>;

class QueryCache final
    : public VideoCommon::QueryCacheLegacy<QueryCache, CachedQuery, CounterStream, HostCounter> {
public:
    QueryCache(RasterizerD3D12& rasterizer, Tegra::MaxwellDeviceMemoryManager& device_memory,
               const Device& device, Scheduler& scheduler);
    ~QueryCache();

    [[nodiscard]] const Device& GetDevice() const noexcept { return device; }
    [[nodiscard]] Scheduler& GetScheduler() const noexcept { return scheduler; }
    [[nodiscard]] bool AnyCommandQueued() const noexcept;

private:
    RasterizerD3D12& rasterizer;
    const Device& device;
    Scheduler& scheduler;
};

class HostCounter final : public VideoCommon::HostCounterBase<QueryCache, HostCounter> {
public:
    HostCounter(QueryCache& cache, std::shared_ptr<HostCounter> dependency,
                VideoCore::QueryType type);
    ~HostCounter();
    void EndQuery();

private:
    u64 BlockingQuery(bool async = false) const override;

    QueryCache& cache;
    VideoCore::QueryType type;
    D3D12_QUERY_TYPE query_type{};
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
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
