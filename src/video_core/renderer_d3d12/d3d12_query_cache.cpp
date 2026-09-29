// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "common/logging.h"

#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {
namespace {

struct QueryInfo {
    D3D12_QUERY_HEAP_TYPE heap;
    D3D12_QUERY_TYPE query;
    u64 bytes;
};

QueryInfo GetQueryInfo(VideoCore::QueryType type) {
    switch (type) {
    case VideoCore::QueryType::SamplesPassed:
        return {D3D12_QUERY_HEAP_TYPE_OCCLUSION, D3D12_QUERY_TYPE_OCCLUSION, sizeof(u64)};
    case VideoCore::QueryType::PrimitivesGenerated:
        return {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
    case VideoCore::QueryType::TfbPrimitivesWritten:
        return {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0,
                sizeof(D3D12_QUERY_DATA_SO_STATISTICS)};
    default:
        UNREACHABLE();
    }
}

} // namespace

QueryPool::QueryPool(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {}

QueryPool::Slot QueryPool::Reserve(VideoCore::QueryType type) {
    std::scoped_lock lock{mutex};
    TypePool& pool = pools[static_cast<size_t>(type)];
    if (pool.free.empty() || !scheduler.IsFree(pool.free.front().tick)) {
        AddPage(type);
    }
    const Slot slot = pool.free.front().slot;
    pool.free.pop_front();
    return slot;
}

void QueryPool::Release(VideoCore::QueryType type, const Slot& slot, u64 tick) {
    std::scoped_lock lock{mutex};
    pools[static_cast<size_t>(type)].free.push_back({slot, tick});
}

void QueryPool::AddPage(VideoCore::QueryType type) {
    const QueryInfo info = GetQueryInfo(type);
    Page page;
    const D3D12_QUERY_HEAP_DESC heap_desc{.Type = info.heap, .Count = PAGE_SIZE, .NodeMask = 0};
    ThrowIfFailed(device.Get()->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&page.heap)),
                  "CreateQueryHeap");
    const D3D12_HEAP_PROPERTIES props{.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = info.bytes * PAGE_SIZE,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    ThrowIfFailed(device.Get()->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&page.readback)),
                  "CreateCommittedResource (query readback)");
    // New slots go first: the free list's front was not free yet.
    TypePool& pool = pools[static_cast<size_t>(type)];
    for (u32 i = PAGE_SIZE; i-- > 0;) {
        pool.free.push_front({Slot{page.heap.Get(), page.readback.Get(), i, info.bytes * i}, 0});
    }
    pool.pages.push_back(std::move(page));
    LOG_DEBUG(Render, "D3D12: query page {} created (type {})", pool.pages.size(),
              static_cast<u32>(type));
}

QueryCache::QueryCache(RasterizerD3D12& rasterizer_,
                       Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device_,
                       Scheduler& scheduler_)
    : QueryCacheLegacy{rasterizer_, device_memory_}, rasterizer{rasterizer_}, device{device_},
      scheduler{scheduler_}, pool{std::make_shared<QueryPool>(device_, scheduler_)} {
    EnableCounters();
    scheduler.RegisterOnSubmit([this] { DisableStreams(); });
    scheduler.RegisterOnReset([this] { EnableCounters(); });
}

QueryCache::~QueryCache() {
    DisableStreams();
    scheduler.ClearSubmissionCallbacks();
    scheduler.Finish();
}

bool QueryCache::AnyCommandQueued() const noexcept {
    return rasterizer.AnyCommandQueued();
}

HostCounter::HostCounter(QueryCache& cache_, std::shared_ptr<HostCounter> dependency_,
                         VideoCore::QueryType type_)
    : HostCounterBase{std::move(dependency_)}, cache{cache_}, type{type_},
      query_type{GetQueryInfo(type_).query}, pool{cache_.GetPool()}, slot{pool->Reserve(type_)} {
    cache.GetScheduler().CommandList()->BeginQuery(slot.heap, query_type, slot.index);
}

HostCounter::~HostCounter() {
    // An open query cannot begin again; the streams end every counter before dropping it, so
    // an unended slot is only left behind at teardown.
    if (ended) {
        pool->Release(type, slot, tick);
    }
}

void HostCounter::EndQuery() {
    if (ended) {
        return;
    }
    auto* list = cache.GetScheduler().CommandList();
    list->EndQuery(slot.heap, query_type, slot.index);
    list->ResolveQueryData(slot.heap, query_type, slot.index, 1, slot.readback, slot.offset);
    tick = cache.GetScheduler().CurrentTick();
    ended = true;
}

u64 HostCounter::BlockingQuery([[maybe_unused]] bool async) const {
    if (!ended) {
        const_cast<HostCounter*>(this)->EndQuery();
    }
    cache.GetScheduler().Wait(tick);
    void* mapped_page{};
    const D3D12_RANGE read_range{static_cast<SIZE_T>(slot.offset),
                                 static_cast<SIZE_T>(slot.offset + GetQueryInfo(type).bytes)};
    ThrowIfFailed(slot.readback->Map(0, &read_range, &mapped_page), "Map (query readback)");
    const u8* const mapped = static_cast<const u8*>(mapped_page) + slot.offset;
    u64 value{};
    switch (type) {
    case VideoCore::QueryType::SamplesPassed:
        std::memcpy(&value, mapped, sizeof(value));
        break;
    case VideoCore::QueryType::PrimitivesGenerated:
        value = reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(mapped)->CPrimitives;
        break;
    case VideoCore::QueryType::TfbPrimitivesWritten:
        value = reinterpret_cast<const D3D12_QUERY_DATA_SO_STATISTICS*>(mapped)->NumPrimitivesWritten;
        break;
    default:
        break;
    }
    const D3D12_RANGE written_range{0, 0};
    slot.readback->Unmap(0, &written_range);
    return value;
}

CachedQuery::CachedQuery(QueryCache&, VideoCore::QueryType, VAddr cpu_addr, u8* host_ptr)
    : CachedQueryBase{cpu_addr, host_ptr} {}

} // namespace D3D12
