// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_heap_packing.h"

int main() {
    using namespace D3D12::HeapPacking;
    constexpr std::uint64_t MiB = 1ULL << 20;
    assert(BlockSize(65 * MiB, 65536, 64 * MiB) == 65 * MiB);
    assert(BlockSize(65 * MiB, 4 * MiB, 64 * MiB) == 68 * MiB);
    assert(BlockSize(1, 4096, 64 * MiB) == 64 * MiB);
    assert(BlockSize(std::numeric_limits<std::uint64_t>::max(), 65536, 0) == 0);
    assert(!Fit(0, 100, 1, 3));
    assert(!Fit(0, 100, 0, 1));
    assert(!Fit(std::numeric_limits<std::uint64_t>::max(), 100, 1, 16));
    // Exhaustive small ranges compared with brute-force placement, including alignment holes.
    std::size_t cases{};
    for (std::uint64_t offset = 0; offset < 100; ++offset)
        for (std::uint64_t bytes = 0; bytes < 80; ++bytes)
            for (std::uint64_t size = 1; size < 40; ++size)
                for (std::uint64_t alignment : {1ULL, 4ULL, 16ULL, 64ULL}) {
                    std::uint64_t expected = offset + bytes;
                    bool found{};
                    for (auto p = offset; p < offset + bytes; ++p) {
                        if (p % alignment == 0 && size <= offset + bytes - p) {
                            expected = p; found = true; break;
                        }
                    }
                    const auto fit = Fit(offset, bytes, size, alignment);
                    assert(fit.has_value() == found);
                    if (found) assert(fit->offset == expected && fit->waste == bytes - size);
                    ++cases;
                }
    std::cout << "Heap packing passed: " << cases << " aligned placements and overflow/MSAA cases\n";
}
