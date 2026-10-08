// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_memory_guard.h"

namespace {
using namespace D3D12;
constexpr std::uint64_t M = MemoryGuard::MiB;
constexpr std::uint64_t limit = 5120 * M;
constexpr std::uint64_t UsedFor(std::uint64_t free) {
    return limit - free;
}

void TestPolicy() {
    MemoryGuardPolicy guard;
    assert(guard.Update(0, 0).stream_bytes == 256 * M);
    assert(!guard.Update(UsedFor(129 * M), limit).trim);
    assert(guard.Update(UsedFor(128 * M), limit).stream_bytes == 128 * M);
    assert(guard.Update(UsedFor(65 * M), limit).stream_bytes == 128 * M);
    assert(guard.Update(UsedFor(64 * M), limit).stream_bytes == 64 * M);
    auto tight = guard.Update(UsedFor(33 * M), limit);
    assert(tight.stream_bytes == 64 * M && !tight.emergency);
    auto emergency = guard.Update(UsedFor(32 * M), limit);
    // Even an emergency keeps the floor ring: without it every upload is a dedicated buffer.
    assert(emergency.emergency && emergency.trim &&
           emergency.stream_bytes == MemoryGuardPolicy::FLOOR_RING);
    assert(guard.Update(limit + M, limit).stream_bytes == MemoryGuardPolicy::FLOOR_RING);

    // 32 -> 64 MiB needs 64 + 256 MiB free for 120 consecutive frames. A missing measurement
    // restarts the count; it never implies headroom.
    for (unsigned i = 0; i < 119; ++i) {
        assert(guard.Update(UsedFor(320 * M), limit).stream_bytes == 32 * M);
    }
    assert(guard.Update(0, 0).stream_bytes == 32 * M);
    for (unsigned i = 0; i < 119; ++i) {
        assert(guard.Update(UsedFor(320 * M), limit).stream_bytes == 32 * M);
    }
    assert(guard.Update(UsedFor(320 * M), limit).stream_bytes == 64 * M);

    // One step at a time: 64 -> 128 needs 384 MiB free, 128 -> 256 needs 512 MiB.
    for (unsigned i = 0; i < 1000; ++i) {
        assert(guard.Update(UsedFor(383 * M), limit).stream_bytes == 64 * M);
    }
    for (unsigned i = 0; i < 119; ++i) {
        assert(guard.Update(UsedFor(384 * M), limit).stream_bytes == 64 * M);
    }
    assert(guard.Update(UsedFor(384 * M), limit).stream_bytes == 128 * M);
    for (unsigned i = 0; i < 119; ++i) {
        assert(guard.Update(UsedFor(4000 * M), limit).stream_bytes == 128 * M);
    }
    assert(guard.Update(UsedFor(4000 * M), limit).stream_bytes == 256 * M);
    // Never beyond the initial ring, however long the headroom lasts.
    for (unsigned i = 0; i < 1000000; ++i) {
        assert(guard.Update(0, limit).stream_bytes == 256 * M);
    }

    // A shrink right after a growth doubles the time the next growth needs.
    assert(guard.GrowFrames() == 120);
    assert(guard.Update(UsedFor(128 * M), limit).stream_bytes == 128 * M);
    assert(guard.GrowFrames() == 240);
    for (unsigned i = 0; i < 239; ++i) {
        assert(guard.Update(UsedFor(512 * M), limit).stream_bytes == 128 * M);
    }
    assert(guard.Update(UsedFor(512 * M), limit).stream_bytes == 256 * M);
    // Two shrinks without a growth in between count once.
    assert(guard.Update(UsedFor(100 * M), limit).stream_bytes == 128 * M);
    assert(guard.Update(UsedFor(50 * M), limit).stream_bytes == 64 * M);
    assert(guard.GrowFrames() == 480);
    // The backoff saturates.
    for (unsigned cycle = 0; cycle < 16; ++cycle) {
        while (guard.Update(UsedFor(4000 * M), limit).stream_bytes != 256 * M) {
        }
        guard.Update(UsedFor(50 * M), limit);
    }
    assert(guard.GrowFrames() == MemoryGuardPolicy::MAX_GROW_FRAMES);
}

void TestRingController() {
    StagingRingController ring;
    auto plan = ring.BeginFrame(UsedFor(1000 * M), limit, 256 * M);
    assert(plan.target == 256 * M && !plan.finish && !ring.Retiring());

    // Shrinking retires the live ring without draining the GPU; no replacement until it is gone
    // and its size plus 64 MiB is free.
    plan = ring.BeginFrame(UsedFor(100 * M), limit, 256 * M);
    assert(plan.previous_target == 256 * M && plan.target == 128 * M);
    assert(ring.Retiring() && !plan.finish && plan.trim);
    assert(ring.WantedRing(256 * M, UsedFor(4000 * M), limit) == 0);
    ring.OnRetired();
    assert(!ring.Retiring());
    assert(ring.WantedRing(0, UsedFor(191 * M), limit) == 0);
    assert(ring.WantedRing(0, UsedFor(192 * M), limit) == 128 * M);
    assert(ring.WantedRing(0, 0, 0) == 0);
    plan = ring.BeginFrame(UsedFor(150 * M), limit, 128 * M);
    assert(!ring.Retiring() && plan.target == 128 * M);

    // An emergency drains the GPU once, while a ring above the floor exists, and replaces it with
    // a floor ring however little is free.
    constexpr std::uint64_t floor = MemoryGuardPolicy::FLOOR_RING;
    plan = ring.BeginFrame(UsedFor(20 * M), limit, 128 * M);
    assert(plan.emergency && plan.finish && plan.target == floor && ring.Retiring());
    plan = ring.BeginFrame(UsedFor(20 * M), limit, 128 * M);
    assert(plan.emergency && !plan.finish);
    ring.OnRetired();
    assert(ring.WantedRing(0, UsedFor(0), limit) == floor);
    assert(ring.WantedRing(0, UsedFor(20 * M), limit) == floor);
    // A floor ring in place is kept, without draining or retiring it.
    plan = ring.BeginFrame(UsedFor(20 * M), limit, floor);
    assert(plan.emergency && !plan.finish && !ring.Retiring());
    assert(ring.WantedRing(floor, UsedFor(20 * M), limit) == 0);
    // Leaving the emergency re-arms the drain for the next one.
    ring.BeginFrame(UsedFor(40 * M), limit, floor);
    plan = ring.BeginFrame(UsedFor(20 * M), limit, 64 * M);
    assert(plan.finish);
    // A refused floor ring still backs off.
    StagingRingController refused;
    refused.BeginFrame(UsedFor(10 * M), limit, 0);
    refused.OnAllocationFailed();
    assert(refused.WantedRing(0, UsedFor(10 * M), limit) == 0);

    // A refused allocation backs off for RETRY_FRAMES frames.
    StagingRingController fresh;
    fresh.BeginFrame(UsedFor(1000 * M), limit, 0);
    assert(fresh.WantedRing(0, UsedFor(1000 * M), limit) == 256 * M);
    fresh.OnAllocationFailed();
    for (unsigned i = 0; i < StagingRingController::RETRY_FRAMES - 1; ++i) {
        fresh.BeginFrame(UsedFor(1000 * M), limit, 0);
        assert(fresh.WantedRing(0, UsedFor(1000 * M), limit) == 0);
    }
    fresh.BeginFrame(UsedFor(1000 * M), limit, 0);
    assert(fresh.WantedRing(0, UsedFor(1000 * M), limit) == 256 * M);
}

void TestHeadroom() {
    HeadroomTracker headroom;
    // Unknown limit (failed query, or no query installed): never blocks, never measures.
    assert(!headroom.NeedsMeasure(4000 * M) && headroom.Fits(4000 * M));
    assert(headroom.Projected(1) == std::numeric_limits<std::uint64_t>::max());

    headroom.Measured(UsedFor(1024 * M), limit);
    assert(!headroom.NeedsMeasure(32 * M) && headroom.Fits(32 * M));
    headroom.Created(700 * M);
    assert(!headroom.NeedsMeasure(64 * M)); // 260 MiB projected: still above reserve + slack
    headroom.Created(140 * M);              // 184 MiB estimated: inside the slack
    assert(headroom.NeedsMeasure(64 * 1024));

    // Inside the slack, small buffers measure once per MEASURE_STEP; large ones every time.
    headroom.Measured(UsedFor(200 * M), limit);
    assert(!headroom.NeedsMeasure(64 * 1024));
    assert(headroom.NeedsMeasure(2 * M));
    headroom.Created(512 * 1024);
    assert(!headroom.NeedsMeasure(64 * 1024));
    headroom.Created(512 * 1024);
    assert(headroom.NeedsMeasure(64 * 1024));

    // The reserve is a hard floor; below it every allocation measures.
    headroom.Measured(UsedFor(100 * M), limit);
    assert(headroom.Fits(36 * M) && !headroom.Fits(36 * M + 1));
    assert(headroom.NeedsMeasure(40 * M));
    headroom.Measured(limit + M, limit);
    assert(!headroom.Fits(1) && headroom.Projected(1) == 0);
}

void TestDedicatedSize() {
    assert(DedicatedStagingSize(0) == 1);
    assert(DedicatedStagingSize(3) == 4);
    assert(DedicatedStagingSize(M) == M);
    assert(DedicatedStagingSize(M + 1) == M + 128 * 1024);
    assert(DedicatedStagingSize(17 * M) == 18 * M);
    assert(DedicatedStagingSize(32 * M) == 32 * M);
    assert(DedicatedStagingSize(33 * M) == 36 * M);
    for (std::uint64_t size = 1; size < 300 * M; size += size / 7 + 4093) {
        const std::uint64_t bytes = DedicatedStagingSize(size);
        assert(bytes >= size && bytes <= std::bit_ceil(size)); // stays in its log2 bucket
        if (size > M) {
            assert(bytes - size < size / 8); // under 12.5 % over the request
        }
    }
}

void TestOptionalCacheGate() {
    OptionalCacheGate gate;
    using Action = OptionalCacheGate::Action;
    assert(gate.Update(UsedFor(200 * M), limit) == Action::None);
    assert(gate.Update(UsedFor(128 * M), limit) == Action::Trim);
    assert(gate.Update(UsedFor(128 * M), limit) == Action::None);
    for (unsigned i = 0; i < 300; ++i) {
        assert(gate.Update(UsedFor(768 * M), limit) == Action::None);
    }
    assert(gate.Update(UsedFor(767 * M), limit) == Action::None); // restarts the count
    assert(gate.Update(0, 0) == Action::None);                    // so does a failed query
    for (unsigned i = 0; i < OptionalCacheGate::RESTORE_FRAMES - 1; ++i) {
        assert(gate.Update(UsedFor(768 * M), limit) == Action::None);
    }
    assert(gate.Update(UsedFor(768 * M), limit) == Action::Restore);
    assert(gate.Update(UsedFor(700 * M), limit) == Action::None);
}

void TestReclaim() {
    struct Buffer {
        unsigned id;
        bool pinned;
        unsigned tick;
    };
    const auto retired = [](const Buffer& b) { return !b.pinned && b.tick == 0; };
    std::vector<Buffer> buffers;
    for (unsigned i = 0; i < 10000; ++i) {
        buffers.push_back({i, i % 3 == 0, i % 2});
    }
    std::size_t cursor = 0;
    unsigned visited = 0;
    for (unsigned sweep = 0; sweep < 2000; ++sweep) {
        unsigned checked = 0;
        ReclaimRetiredStaging(buffers, cursor, [&](const Buffer& b) {
            ++checked;
            ++visited;
            return retired(b);
        });
        assert(checked <= 16 && cursor <= buffers.size());
    }
    assert(buffers.size() == 6667 && visited <= 32000);
    for (const auto& b : buffers) {
        assert(!retired(b));
    }

    // Recovery passes no bound: one call removes every retired entry.
    std::vector<Buffer> all;
    for (unsigned i = 0; i < 10000; ++i) {
        all.push_back({i, i % 3 == 0, i % 2});
    }
    cursor = 0;
    ReclaimRetiredStaging(all, cursor, retired, std::numeric_limits<std::size_t>::max());
    assert(all.size() == 6667 && cursor == 0);
}
} // Anonymous namespace

int main() {
    TestPolicy();
    TestRingController();
    TestHeadroom();
    TestDedicatedSize();
    TestOptionalCacheGate();
    TestReclaim();
    std::cout << "PASS: ring policy (thresholds, staged growth, backoff), ring controller (retire, "
                 "drain once, backoff), allocation headroom, dedicated sizes, optional cache gate "
                 "and bounded reclaim\n";
}
