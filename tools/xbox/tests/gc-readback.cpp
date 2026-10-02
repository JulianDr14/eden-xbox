// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_gc_readback.h"
using namespace D3D12;
int main() {
    constexpr u64 cap = 8ULL << 20;
    std::mt19937 random{0x12345678};
    for (unsigned trial = 0; trial < 20000; ++trial) {
        std::vector<GcReadbackRegion> regions;
        u64 source_end = 0, tight_end = 0;
        for (unsigned i = 0, count = 1 + random()%12; i < count; ++i) {
            const u32 row_bytes = 1 + random()%1100;
            const u32 pitch = (row_bytes + 255) & ~255U;
            const auto region = PlanGcReadbackRegion(source_end, tight_end, row_bytes, pitch,
                                                     1 + random()%15, 1 + random()%4, cap);
            assert(region);
            assert(region->source_offset % 512 == 0);
            regions.push_back(*region);
            source_end = region->source_offset + region->source_bytes;
            tight_end += region->tight_bytes;
        }
        std::vector<u8> actual(source_end + 16, 0xcd), expected(tight_end);
        for (const auto& region : regions) {
            for (u64 row = 0; row < u64{region.rows}*region.depth; ++row) {
                for (u32 col = 0; col < region.row_bytes; ++col) {
                    const u8 value = static_cast<u8>(random());
                    actual[region.source_offset + row*region.row_pitch + col] = value;
                    expected[region.tight_offset + row*region.row_bytes + col] = value;
                }
            }
        }
        for (const auto& region : regions) CompactGcReadback(actual, region);
        assert(std::equal(expected.begin(), expected.end(), actual.begin()));
        assert(std::all_of(actual.begin()+source_end, actual.end(), [](u8 byte){return byte==0xcd;}));
    }
    assert(PlanGcReadbackRegion(0, 0, 256, 256, 32768, 1, cap));
    assert(!PlanGcReadbackRegion(0, 0, 256, 256, 32769, 1, cap));
    assert(!PlanGcReadbackRegion(~u64{0}, 0, 256, 256, 1, 1, ~u64{0}));
    assert(!PlanGcReadbackRegion(0, 1, 256, 256, 1, 1, cap));
    assert(!PlanGcReadbackRegion(0, 0, 257, 256, 1, 1, cap));
    assert(!PlanGcReadbackRegion(0, 0, 16, 255, 1, 1, cap));
    assert(!PlanGcReadbackRegion(0, 0, 16, 256, 0, 1, cap));
    std::cout << "PASS 20000 randomized mip/layer/volume row layouts, overlap, guards, cap and overflow\n";
}