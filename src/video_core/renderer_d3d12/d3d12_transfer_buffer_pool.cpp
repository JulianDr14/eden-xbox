// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/alignment.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"

namespace D3D12 {

namespace {

/// Buffers are placed at 64 KiB anyway: sizing them so lets similar copies share one.
constexpr u64 SIZE_GRANULARITY = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

} // Anonymous namespace

TransferBufferPool::TransferBufferPool(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {}

TransferBufferPool::~TransferBufferPool() = default;

ComPtr<ID3D12Resource> TransferBufferPool::Acquire(u64 size, bool unordered_access) {
    size = Common::AlignUp(std::max<u64>(size, 1), SIZE_GRANULARITY);
    {
        std::scoped_lock lock{mutex};
        // Best fit among the idle buffers, and at most twice the size so a large buffer is not
        // tied up by small copies.
        auto best = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->size < size || it->size > size * 2 ||
                (unordered_access && !it->unordered_access) || !scheduler.IsFree(it->tick)) {
                continue;
            }
            if (best == entries.end() || it->size < best->size) {
                best = it;
            }
        }
        if (best != entries.end()) {
            ComPtr<ID3D12Resource> buffer = std::move(best->buffer);
            pooled_bytes -= best->size;
            *best = std::move(entries.back());
            entries.pop_back();
            return buffer;
        }
    }
    return CreateCommittedBuffer(device.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_COMMON,
                                 unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                  : D3D12_RESOURCE_FLAG_NONE,
                                 "Create transfer buffer");
}

void TransferBufferPool::Release(ComPtr<ID3D12Resource>&& buffer) {
    if (!buffer) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = buffer->GetDesc();
    {
        std::scoped_lock lock{mutex};
        if (pooled_bytes + desc.Width <= MAX_POOLED_BYTES) {
            pooled_bytes += desc.Width;
            entries.push_back({
                .buffer = std::move(buffer),
                .size = desc.Width,
                .unordered_access = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
                .tick = scheduler.CurrentTick(),
            });
            return;
        }
    }
    scheduler.DeferRelease(std::move(buffer));
}

void TransferBufferPool::Trim(bool under_pressure) {
    const u64 current = scheduler.CurrentTick();
    std::scoped_lock lock{mutex};
    std::erase_if(entries, [&](Entry& entry) {
        const bool idle_too_long = current > entry.tick + IDLE_TICKS;
        if (!under_pressure && !idle_too_long) {
            return false;
        }
        pooled_bytes -= entry.size;
        // Not necessarily done on the GPU yet: release it after the work recorded so far.
        scheduler.DeferRelease(std::move(entry.buffer));
        return true;
    });
}

u64 TransferBufferPool::PooledBytes() const {
    std::scoped_lock lock{mutex};
    return pooled_bytes;
}

} // namespace D3D12
