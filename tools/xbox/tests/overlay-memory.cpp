// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <iostream>
#include "video_core/renderer_d3d12/d3d12_overlay_memory.h"

int main() {
    using namespace D3D12;
    constexpr u64 MiB = 1024ULL * 1024;
    assert(FormatOverlayResidentMemory(std::nullopt) == "RAM WS --");
    assert(FormatOverlayResidentMemory(0) == "RAM WS 0 MIB");
    assert(FormatOverlayResidentMemory(512 * MiB) == "RAM WS 512 MIB");
    assert(FormatOverlayResidentMemory(1023 * MiB) == "RAM WS 1023 MIB");
    assert(FormatOverlayResidentMemory(1024 * MiB) == "RAM WS 1.00 GIB");
    assert(FormatOverlayResidentMemory(4608 * MiB) == "RAM WS 4.50 GIB");
    assert(!QueryOverlayResidentMemory(nullptr));
    const auto resident = QueryOverlayResidentMemory();
    assert(resident && *resident > 0);
    constexpr int samples = 10000;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < samples; ++i) {
        assert(QueryOverlayResidentMemory().has_value());
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::cout << "PASS; resident=" << *resident << " bytes; query average="
              << std::chrono::duration<double, std::micro>(elapsed).count() / samples
              << " us (desktop microbenchmark, not gameplay)\n";
}
