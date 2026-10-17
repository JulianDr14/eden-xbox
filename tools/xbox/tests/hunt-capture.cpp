// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// cl /nologo /std:c++20 /EHsc /DNOMINMAX /I src tools\xbox\tests\hunt-capture.cpp
#include "video_core/renderer_d3d12/diagnostics/hunt_capture.h"

#include <cstdio>
#include <cstdlib>

using namespace D3D12::Hunt;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

int main() {
    Capture capture;
    capture.Reset(3, 4);
    const int target_a = 0, target_b = 0;
    const Pass a{.id = &target_a, .width = 1280, .height = 720, .samples = 1, .colors = 1,
                 .color_format = 28, .depth_format = 0};
    const Pass b{.id = &target_b, .width = 1280, .height = 720, .samples = 1, .colors = 1,
                 .color_format = 28, .depth_format = 0};
    check(capture.InternPass(a) == 0, "first pass");
    check(capture.InternPass(b) == 1, "equal targets in different framebuffers stay apart");
    check(capture.InternPass(a) == 0, "known pass found again");

    // List 7: a baseline, then two draws. List 8 starts with a draw whose list had no baseline.
    check(!capture.HasBaseline(7), "no baseline before the first timestamp");
    check(capture.TakeTimestamp(7) == 0u, "baseline index");
    check(capture.HasBaseline(7), "baseline belongs to list 7");
    Record first{.pass = 0, .count = 3, .instances = 1, .timestamp = *capture.TakeTimestamp(7)};
    Record second{.pass = 0, .count = 6, .instances = 1, .timestamp = *capture.TakeTimestamp(7)};
    Record other_list{.pass = 1, .count = 6, .instances = 1, .timestamp = *capture.TakeTimestamp(8)};
    check(!capture.TakeTimestamp(8).has_value(), "timestamps stop at capacity");
    first.outcome = second.outcome = other_list.outcome = Outcome::Recorded;
    capture.Add(first);
    capture.Add(second);
    capture.Add(other_list);
    capture.Add(Record{.outcome = Outcome::Culled});
    check(capture.Records().size() == 3 && capture.Full(), "records stop at capacity");
    capture.EndFrame(16.7);
    check(capture.Frame() == 1, "frame counted");

    // 1 MHz clock: one tick is 1000 ns.
    const u64 values[] = {100, 103, 110, 50};
    check(capture.GpuNs(first, values, 1'000'000) == 3000u, "delta from the baseline");
    check(capture.GpuNs(second, values, 1'000'000) == 7000u, "delta from the previous draw");
    check(!capture.GpuNs(other_list, values, 1'000'000), "no delta across lists");
    check(!capture.GpuNs(Record{}, values, 1'000'000), "no timestamp, no time");
    check(!capture.GpuNs(first, {}, 1'000'000), "unresolved timestamps, no time");

    const std::string text = capture.Serialize(2, values, 1'000'000, "perf Draws 2\n");
    check(text.starts_with("hunt 1 capture 2 frames 1 records 3 dropped 1 timestamps 4\n"),
          "header");
    check(text.find("frame 0 16.700\n") != std::string::npos, "frame line");
    check(text.find("pass 1 1280x720 s1 c1 f28 d0\n") != std::string::npos, "pass line");
    check(text.find("perf Draws 2\n") != std::string::npos, "extra lines");
    check(text.find(" 3000 0\n") != std::string::npos, "record with GPU time");
    check(text.find(" - 0\n") != std::string::npos, "record with unknown GPU time");
    check(text.ends_with("end\n"), "terminator");

    capture.Reset(2, 2);
    check(capture.Records().empty() && capture.Passes().empty() && capture.Timestamps() == 0 &&
              capture.Frame() == 0,
          "reset");
    std::puts("hunt capture: passes, timestamps, GPU deltas and serialization PASS");
    return 0;
}
