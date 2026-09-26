// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

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

QueryCache::QueryCache(RasterizerD3D12& rasterizer_,
                       Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device_,
                       Scheduler& scheduler_)
    : QueryCacheLegacy{rasterizer_, device_memory_}, rasterizer{rasterizer_}, device{device_},
      scheduler{scheduler_} {
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
    : HostCounterBase{std::move(dependency_)}, cache{cache_}, type{type_} {
    const QueryInfo info = GetQueryInfo(type);
    query_type = info.query;
    const D3D12_QUERY_HEAP_DESC heap_desc{.Type = info.heap, .Count = 1, .NodeMask = 0};
    ThrowIfFailed(cache.GetDevice().Get()->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap)),
                  "CreateQueryHeap");
    const D3D12_HEAP_PROPERTIES props{.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = info.bytes,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    ThrowIfFailed(cache.GetDevice().Get()->CreateCommittedResource(
                      &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                      IID_PPV_ARGS(&readback)),
                  "CreateCommittedResource (query readback)");
    cache.GetScheduler().CommandList()->BeginQuery(heap.Get(), query_type, 0);
}

HostCounter::~HostCounter() = default;

void HostCounter::EndQuery() {
    if (ended) {
        return;
    }
    auto* list = cache.GetScheduler().CommandList();
    list->EndQuery(heap.Get(), query_type, 0);
    list->ResolveQueryData(heap.Get(), query_type, 0, 1, readback.Get(), 0);
    tick = cache.GetScheduler().CurrentTick();
    ended = true;
}

u64 HostCounter::BlockingQuery([[maybe_unused]] bool async) const {
    if (!ended) {
        const_cast<HostCounter*>(this)->EndQuery();
    }
    cache.GetScheduler().Wait(tick);
    void* mapped{};
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback->GetDesc().Width)};
    ThrowIfFailed(readback->Map(0, &read_range, &mapped), "Map (query readback)");
    u64 value{};
    switch (type) {
    case VideoCore::QueryType::SamplesPassed:
        std::memcpy(&value, mapped, sizeof(value));
        break;
    case VideoCore::QueryType::PrimitivesGenerated:
        value = static_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(mapped)->CPrimitives;
        break;
    case VideoCore::QueryType::TfbPrimitivesWritten:
        value = static_cast<const D3D12_QUERY_DATA_SO_STATISTICS*>(mapped)->NumPrimitivesWritten;
        break;
    default:
        break;
    }
    const D3D12_RANGE written_range{0, 0};
    readback->Unmap(0, &written_range);
    return value;
}

CachedQuery::CachedQuery(QueryCache&, VideoCore::QueryType, VAddr cpu_addr, u8* host_ptr)
    : CachedQueryBase{cpu_addr, host_ptr} {}

} // namespace D3D12
