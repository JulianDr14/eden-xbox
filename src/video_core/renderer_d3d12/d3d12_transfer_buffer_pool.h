// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;

/// Reuses the DEFAULT-heap buffers that copies go through (texture reinterpretations, plane
/// splits, row repacks; copies within one cached buffer) instead of creating a committed
/// resource for every copy and layer. One pool, owned by the renderer, serves both caches.
///
/// A released buffer is handed out again once the GPU has finished the submission that used it.
/// It is in COMMON then, like a new one: buffers decay to COMMON at the end of every
/// ExecuteCommandLists. Callers therefore keep the barriers they record for a fresh buffer.
class TransferBufferPool {
public:
    explicit TransferBufferPool(const Device& device, Scheduler& scheduler);
    ~TransferBufferPool();

    TransferBufferPool(const TransferBufferPool&) = delete;
    TransferBufferPool& operator=(const TransferBufferPool&) = delete;

    /// A buffer of at least size bytes in COMMON (with UAV access when unordered_access).
    [[nodiscard]] ComPtr<ID3D12Resource> Acquire(u64 size, bool unordered_access = false);

    /// Returns a buffer once the commands recorded so far are done with it. Buffers past the pool's
    /// budget are released instead.
    void Release(ComPtr<ID3D12Resource>&& buffer);

    /// Frees idle buffers: all of them under memory pressure, otherwise the ones unused for a while.
    void Trim(bool under_pressure);

    [[nodiscard]] u64 PooledBytes() const;

private:
    struct Entry {
        ComPtr<ID3D12Resource> buffer;
        u64 size;
        bool unordered_access;
        u64 tick; ///< scheduler tick of the last submission that used it
    };

    /// Idle buffers above this are released: the pool must not grow into the texture budget.
    static constexpr u64 MAX_POOLED_BYTES = 64ULL << 20;
    /// Submissions an idle buffer is kept for when there is no memory pressure.
    static constexpr u64 IDLE_TICKS = 2048;

    const Device& device;
    Scheduler& scheduler;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    u64 pooled_bytes{};
};

} // namespace D3D12
