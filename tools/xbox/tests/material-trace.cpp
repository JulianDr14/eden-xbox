// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <iostream>
#include <limits>
#include "video_core/renderer_d3d12/diagnostics/d3d12_material_trace.h"

int main() {
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
