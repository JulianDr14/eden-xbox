// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <iostream>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_memory_guard.h"

int main() {
    using Policy = D3D12::MemoryGuardPolicy;
    constexpr auto M = Policy::MiB;
    constexpr auto limit = 5120 * M;
    Policy guard;
    assert(guard.Update(0, 0).stream_bytes == 256 * M);
    assert(!guard.Update(limit - 129 * M, limit).trim);
    assert(guard.Update(limit - 128 * M, limit).stream_bytes == 128 * M);
    assert(guard.Update(limit - 65 * M, limit).stream_bytes == 128 * M);
    assert(guard.Update(limit - 64 * M, limit).stream_bytes == 64 * M);
    assert(guard.Update(limit - 11 * M, limit).stream_bytes == 64 * M);
    auto emergency = guard.Update(limit - 10 * M, limit);
    assert(emergency.emergency && emergency.trim && emergency.stream_bytes == 0);
    assert(guard.Update(limit + M, limit).emergency);
    for (unsigned i = 0; i < 119; ++i) {
        assert(guard.Update(limit - 256 * M, limit).stream_bytes == 0);
    }
    // Missing measurements reset recovery; they never imply unlimited memory or healthy frames.
    assert(guard.Update(0, 0).stream_bytes == 0);
    for (unsigned i = 0; i < 119; ++i) {
        assert(guard.Update(limit - 256 * M, limit).stream_bytes == 0);
    }
    assert(guard.Update(limit - 256 * M, limit).stream_bytes == 64 * M);
    for (unsigned i = 0; i < 1000000; ++i) {
        assert(guard.Update(0, limit).stream_bytes == 64 * M);
    }
    assert(guard.Update(limit - 10 * M, limit).stream_bytes == 0);
    struct Buffer { unsigned id; bool pinned; unsigned tick; };
    std::vector<Buffer> buffers;
    for (unsigned i = 0; i < 10000; ++i) {
        buffers.push_back({i, i % 3 == 0, i % 2});
    }
    std::size_t cursor = 0;
    unsigned visited = 0;
    for (unsigned sweep = 0; sweep < 2000; ++sweep) {
        unsigned checked = 0;
        D3D12::ReclaimRetiredStaging(buffers, cursor, [&](const Buffer& b) {
            ++checked;
            ++visited;
            return !b.pinned && b.tick == 0;
        });
        assert(checked <= 16 && cursor <= buffers.size());
    }
    assert(buffers.size() == 6667 && visited <= 32000);
    for (const auto& b : buffers) {
        assert(b.pinned || b.tick != 0);
    }
    std::cout << "Memory guard thresholds, over-limit, missing query, recovery hysteresis and "
                 "no automatic budget growth passed\n";
}
