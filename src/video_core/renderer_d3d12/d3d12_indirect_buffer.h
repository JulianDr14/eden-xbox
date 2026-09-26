// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;
class StagingBufferPool;

/// GPU memory for the ExecuteIndirect argument buffers the rasterizer assembles. The guest's
/// indirect records cannot be used in place: every draw also has to set the runtime-data root
/// constants spirv_to_dxil reads (first vertex, base instance...), and a command signature takes
/// those from the same record, ahead of the draw arguments. So the records are rebuilt here with
/// GPU copies, which also sees arguments the guest wrote from shaders.
///
/// Default-heap chunks, reused once the GPU is past the list that last wrote them.
class IndirectArgumentRing {
public:
    struct Range {
        ID3D12Resource* buffer;
        u64 offset;
    };

    IndirectArgumentRing(const Device& device, Scheduler& scheduler, StagingBufferPool& staging);
    ~IndirectArgumentRing();

    IndirectArgumentRing(const IndirectArgumentRing&) = delete;
    IndirectArgumentRing& operator=(const IndirectArgumentRing&) = delete;

    /// size bytes in COPY_DEST, for the caller's copies.
    [[nodiscard]] Range Allocate(u64 size);
    /// Like Allocate, filled with words first (the values the CPU knows).
    [[nodiscard]] Range Upload(std::span<const u32> words);
    /// Moves the range's buffer to INDIRECT_ARGUMENT once its copies are recorded.
    void Ready(const Range& range);

private:
    struct Chunk {
        ComPtr<ID3D12Resource> resource;
        u64 size{};
        u64 cursor{};
        u64 last_tick{};
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        u64 state_tick{}; ///< buffers decay to COMMON when their list ends
    };

    Chunk& FindChunk(u64 size);
    Chunk* ChunkOf(ID3D12Resource* resource);
    void Transition(Chunk& chunk, D3D12_RESOURCE_STATES next);

    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    std::vector<Chunk> chunks;
    size_t current{};
};

} // namespace D3D12
