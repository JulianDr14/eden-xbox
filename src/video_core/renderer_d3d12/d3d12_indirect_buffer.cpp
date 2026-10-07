// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_indirect_buffer.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {

namespace {
constexpr u64 CHUNK_SIZE = 64 * 1024;
constexpr u64 ALIGNMENT = 16;
} // Anonymous namespace

IndirectArgumentRing::IndirectArgumentRing(const Device& device_, Scheduler& scheduler_,
                                           StagingBufferPool& staging_)
    : device{device_}, scheduler{scheduler_}, staging{staging_} {}

IndirectArgumentRing::~IndirectArgumentRing() {
    for (Chunk& chunk : chunks) {
        scheduler.DeferRelease(std::move(chunk.resource));
    }
}

IndirectArgumentRing::Chunk& IndirectArgumentRing::FindChunk(u64 size) {
    const u64 tick = scheduler.CurrentTick();
    if (current < chunks.size()) {
        Chunk& chunk = chunks[current];
        if (Common::AlignUp(chunk.cursor, ALIGNMENT) + size <= chunk.size) {
            chunk.cursor = Common::AlignUp(chunk.cursor, ALIGNMENT);
            chunk.last_tick = tick;
            return chunk;
        }
    }
    for (size_t index = 0; index < chunks.size(); ++index) {
        Chunk& chunk = chunks[index];
        if (chunk.size >= size && chunk.last_tick != tick &&
            scheduler.IsFree(chunk.last_tick)) {
            chunk.cursor = 0;
            chunk.last_tick = tick;
            current = index;
            return chunk;
        }
    }
    Chunk& chunk = chunks.emplace_back();
    chunk.size = std::max(CHUNK_SIZE, Common::AlignUp(size, CHUNK_SIZE));
    chunk.resource = CreateCommittedBuffer(device.Get(), chunk.size, D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE,
                                           "CreateCommittedResource (indirect arguments)");
    chunk.last_tick = tick;
    current = chunks.size() - 1;
    LOG_DEBUG(Render, "D3D12: indirect argument chunk {} ({} KiB)", current, chunk.size / 1024);
    return chunk;
}

IndirectArgumentRing::Chunk* IndirectArgumentRing::ChunkOf(ID3D12Resource* resource) {
    for (Chunk& chunk : chunks) {
        if (chunk.resource.Get() == resource) {
            return &chunk;
        }
    }
    return nullptr;
}

void IndirectArgumentRing::Transition(Chunk& chunk, D3D12_RESOURCE_STATES next) {
    const u64 tick = scheduler.CurrentTick();
    const D3D12_RESOURCE_STATES before =
        chunk.state_tick == tick ? chunk.state : D3D12_RESOURCE_STATE_COMMON;
    if (before != next) {
        const D3D12_RESOURCE_BARRIER barrier{
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {.pResource = chunk.resource.Get(),
                           .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           .StateBefore = before,
                           .StateAfter = next}};
        scheduler.CommandList()->ResourceBarrier(1, &barrier);
    }
    chunk.state = next;
    chunk.state_tick = tick;
}

IndirectArgumentRing::Range IndirectArgumentRing::Allocate(u64 size) {
    Chunk& chunk = FindChunk(size);
    Transition(chunk, D3D12_RESOURCE_STATE_COPY_DEST);
    const Range range{chunk.resource.Get(), chunk.cursor};
    chunk.cursor += size;
    return range;
}

IndirectArgumentRing::Range IndirectArgumentRing::Upload(std::span<const u32> words) {
    const u64 size = words.size_bytes();
    const StagingBufferRef upload = staging.Request(size, MemoryUsage::Upload);
    std::memcpy(upload.mapped_span.data(), words.data(), size);
    const Range range = Allocate(size);
    scheduler.CommandList()->CopyBufferRegion(range.buffer, range.offset, upload.buffer,
                                              upload.offset, size);
    return range;
}

void IndirectArgumentRing::Ready(const Range& range) {
    if (Chunk* const chunk = ChunkOf(range.buffer)) {
        Transition(*chunk, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
}

} // namespace D3D12
