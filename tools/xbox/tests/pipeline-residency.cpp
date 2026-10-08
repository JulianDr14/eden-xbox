// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Pipeline residency gate: idle limits against the app's headroom, and the hot pipeline set's
// sessions, decay, prewarm choice and file round trip. Run with
// build-uwp/diagnostics/pipeline-residency/run.bat.

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_pipeline_residency.h"

namespace {
using namespace D3D12;
constexpr std::uint64_t M = MemoryGuard::MiB;
constexpr std::uint64_t limit = 5120 * M;

void TestIdleFrames() {
    using P = PipelineResidencyPolicy;
    assert(P::IdleFrames(0, 0) == P::IDLE_FRAMES);
    assert(P::IdleFrames(limit - 1024 * M, limit) == P::IDLE_FRAMES);
    assert(P::IdleFrames(limit - 129 * M, limit) == P::IDLE_FRAMES);
    assert(P::IdleFrames(limit - 128 * M, limit) == P::LOW_IDLE_FRAMES);
    assert(P::IdleFrames(limit - 33 * M, limit) == P::LOW_IDLE_FRAMES);
    assert(P::IdleFrames(limit - 32 * M, limit) == P::EMERGENCY_IDLE_FRAMES);
    assert(P::IdleFrames(limit + M, limit) == P::EMERGENCY_IDLE_FRAMES);
}

/// Saves the set and starts the next session from it, as a new boot does.
HotPipelineSet NextSession(const HotPipelineSet& set) {
    HotPipelineSet next;
    const std::vector<std::uint8_t> bytes = set.Serialize();
    next.Load(bytes);
    return next;
}

void TestHotSet() {
    // No file, or a damaged one: the start of the disk cache is prewarmed.
    HotPipelineSet set;
    set.Load({});
    assert(!set.HasHistory());
    assert(set.Prewarm(123, 0) && set.Prewarm(123, HotPipelineSet::FIRST_SESSION_PREWARM - 1));
    assert(!set.Prewarm(123, HotPipelineSet::FIRST_SESSION_PREWARM));
    const std::vector<std::uint8_t> garbage{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    set.Load(garbage);
    assert(!set.HasHistory());

    // A session marks what it draws, once.
    assert(set.MarkUsed(10) && !set.MarkUsed(10) && set.MarkUsed(20));
    set = NextSession(set);
    assert(set.HasHistory() && set.Size() == 2);
    assert(set.Prewarm(10, 999999) && set.Prewarm(20, 999999) && !set.Prewarm(30, 0));
    // Drawn again in a later session: marked again, so it is saved with the new session.
    assert(set.MarkUsed(10));

    // A pipeline not drawn for KEEP_SESSIONS sessions is forgotten; one drawn again stays.
    for (std::uint32_t i = 0; i < HotPipelineSet::KEEP_SESSIONS; ++i) {
        set = NextSession(set);
        set.MarkUsed(10);
    }
    assert(set.Prewarm(10, 0));
    assert(!set.Prewarm(20, 0));

    // A damaged length is rejected whole.
    std::vector<std::uint8_t> bytes = set.Serialize();
    bytes.pop_back();
    HotPipelineSet truncated;
    truncated.Load(bytes);
    assert(!truncated.HasHistory());

    // At most MAX_PREWARM pipelines are built at boot, the most recently drawn first.
    HotPipelineSet big;
    big.Load({});
    for (std::uint64_t hash = 0; hash < HotPipelineSet::MAX_PREWARM; ++hash) {
        big.MarkUsed(1'000'000 + hash); // the older session
    }
    big = NextSession(big);
    for (std::uint64_t hash = 0; hash < 100; ++hash) {
        big.MarkUsed(hash); // the newer one
    }
    big = NextSession(big);
    assert(big.Size() == HotPipelineSet::MAX_PREWARM + 100);
    assert(big.PrewarmSize() == HotPipelineSet::MAX_PREWARM);
    for (std::uint64_t hash = 0; hash < 100; ++hash) {
        assert(big.Prewarm(hash, 0));
    }
    std::size_t old_prewarmed = 0;
    for (std::uint64_t hash = 0; hash < HotPipelineSet::MAX_PREWARM; ++hash) {
        old_prewarmed += big.Prewarm(1'000'000 + hash, 0) ? 1 : 0;
    }
    assert(old_prewarmed == HotPipelineSet::MAX_PREWARM - 100);
}

} // namespace

int main() {
    TestIdleFrames();
    TestHotSet();
    std::cout << "PASS: idle limits by headroom, hot pipeline sessions, decay, prewarm cap and file "
                 "round trip\n";
    return 0;
}
