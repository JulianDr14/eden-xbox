// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <iostream>
#include <limits>
#include "video_core/renderer_d3d12/diagnostics/d3d12_material_trace.h"

int main() {
    for (std::uint32_t format = 0; format < 120; ++format) {
        assert(D3D12::MaterialProbeRegionBytes(format) ==
               (format == 2 ? 16 : format == 26 ? 4 : 0));
    }
    D3D12::MaterialSampledInputBudget inputs;
    assert(!inputs.Fits(0));
    assert(!inputs.Fits(std::numeric_limits<std::uint64_t>::max()));
    assert(!inputs.Commit(inputs.MAX_RESOURCE_BYTES + 1));
    // Four RGBA8/RGB10/RG16/R11 1920x1080 footprints, then remaining space.
    constexpr std::uint64_t screen_bytes = 1920ULL * 1080 * 4;
    for (int i = 0; i < 4; ++i) assert(inputs.Commit(screen_bytes));
    const auto remaining = inputs.MAX_BYTES - 4 * screen_bytes;
    assert(!inputs.Commit(remaining + 1));
    assert(inputs.Commit(remaining));
    assert(!inputs.Commit(1));
    inputs = {};
    assert(inputs.Commit(inputs.MAX_RESOURCE_BYTES));
    D3D12::MaterialSnapshotSelection snapshots;
    assert(!snapshots.Consume(1) && !snapshots.Consume(0));
    snapshots.Begin({7, 8});
    assert(!snapshots.Consume(9));
    assert(snapshots.Consume(7) && !snapshots.Consume(7));
    assert(!snapshots.AllowBytes(0));
    assert(!snapshots.AllowBytes(std::numeric_limits<std::uint64_t>::max()));
    assert(snapshots.AllowBytes(snapshots.MAX_BYTES - 1));
    assert(!snapshots.AllowBytes(2));
    assert(snapshots.Consume(8) && snapshots.AllowBytes(1));
    assert(!snapshots.AllowBytes(1));
    snapshots.Begin({7, 7});
    assert(snapshots.Consume(7) && !snapshots.Consume(7));
    assert(snapshots.AllowBytes(snapshots.MAX_BYTES));
    // A selected draw's four 1080p MRTs fit; a fifth cannot exceed the shared cap.
    snapshots.Begin({7, 0}, {2712, 0});
    assert(snapshots.Consume(7, 2712));
    for (int i = 0; i < 4; ++i) assert(snapshots.AllowBytes(screen_bytes));
    assert(!snapshots.AllowBytes(screen_bytes));
    snapshots.Begin({0, 0});
    assert(!snapshots.Consume(0) && !snapshots.Consume(7));
    snapshots.Begin({7, 7}, {192, 7929});
    assert(!snapshots.Matches(7, 102) && !snapshots.Consume(7, 102));
    assert(snapshots.Targeted(7, 7929) && snapshots.Consume(7, 7929));
    assert(!snapshots.Matches(7, 7929));
    assert(snapshots.Targeted(7, 192) && snapshots.Consume(7, 192));
    snapshots.Begin({7, 8});
    assert(snapshots.Matches(7, 7929) && !snapshots.Targeted(7, 7929));
    assert(snapshots.Consume(7, 7929));
    for (std::uint32_t f = 0; f < 120; ++f) {
        const auto expected = (f >= 70 && f <= 84) || (f >= 94 && f <= 99);
        assert((D3D12::MaterialBlockBytes(f) != 0) == expected);
    }
    assert(D3D12::MaterialBlockBytes(70) == 8 && D3D12::MaterialBlockBytes(81) == 8);
    assert(D3D12::MaterialBlockBytes(84) == 16 && D3D12::MaterialBlockBytes(99) == 16);
    D3D12::MaterialTraceBudget trace;
    assert(!trace.Select(1) && !trace.AllowBytes(1));
    trace.Begin(7);
    assert(!trace.Select(6) && trace.matched == 0);
    assert(trace.Select(7));
    assert(trace.AllowBytes(trace.MAX_BYTES - 10));
    assert(trace.AllowBytes(10));
    assert(!trace.Select(7) && trace.matched == 2 && trace.selected == 1);
    assert(!trace.AllowBytes(std::numeric_limits<std::size_t>::max()));
    assert(trace.bytes == trace.MAX_BYTES && trace.dropped_lines == 1);
    trace.End();
    assert(!trace.Select(7));
    trace.Begin();
    for (std::size_t draw = 0; draw < trace.MAX_DRAWS; ++draw) {
        assert(trace.Select(draw));
    }
    assert(!trace.Select(0) && trace.matched == trace.MAX_DRAWS + 1);
    trace.Begin();
    assert(trace.matched == 0 && trace.bytes == 0 && trace.dropped_lines == 0);
    assert(trace.Select(99));
    assert(!trace.AllowBytes(trace.MAX_BYTES + 1));
    assert(!trace.Select(99));
    std::cout << "PASS material trace: opt-in, shader selection, draw/byte bounds, overflow, reset\n";
}
