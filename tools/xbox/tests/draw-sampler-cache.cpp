// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <iostream>
#include "video_core/renderer_d3d12/d3d12_draw_sampler_cache.h"

int main() {
    std::array<u32, 64> table{};
    u32 random = 19;
    u64 requests{}, resolved{}, reused{};
    for (u32 draw = 0; draw < 10000; ++draw) {
        // Guest writes and descriptor-table changes between draws must always be visible.
        for (u32 i = 0; i < table.size(); ++i) table[i] = draw * 67 + i;
        for (bool enabled : {false, true}) {
            D3D12::DrawSamplerCache<u32> cache{enabled};
            u32 calls{};
            const auto resolve = [&](u32 index) {
                ++calls;
                return index < table.size() ? table[index] : 0;
            };
            for (u32 binding = 0; binding < 128; ++binding) {
                random = random * 1664525 + 1013904223;
                const u32 index = binding < 32 ? binding % 4 : binding < 64 ? 4 * (binding % 8) :
                                  binding < 96 ? (random >> 16) % 64 : ~u32{0};
                const u32 expected = index < table.size() ? table[index] : 0;
                assert(cache.Get(index, resolve) == expected);
            }
            assert(calls + cache.Hits() == 128);
            if (!enabled) assert(calls == 128 && cache.Hits() == 0);
            requests += 128; resolved += calls; reused += cache.Hits();
        }
    }
    assert(requests == resolved + reused);
    std::cout << "PASS " << requests << " sampler requests: full-key collision checks, repeated/unique/invalid indices, "
              << "guest writes and table changes between draws, disabled path; reused " << reused << '\n';
}
