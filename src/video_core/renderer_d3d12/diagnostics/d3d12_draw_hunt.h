// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>

#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/diagnostics/hunt_capture.h"

namespace D3D12 {

class Framebuffer;
class Scheduler;

/// Captures the next `frames` presented frames into LogDir/jadyx_hunt.txt (any thread), and
/// samples the CPU into jadyx_hunt_profile.txt meanwhile. Read on the PC with
/// tools/xbox/hunt-report.py.
void HuntNextFrames(u32 frames = 8);

/// The D3D12 side of the bottleneck hunt (hunt_capture.h): a timestamp after every recorded
/// call while a capture runs, resolved once at its end and written when the GPU is done with
/// them. Idle, it costs each call one branch. GPU thread only.
class DrawHunt {
public:
    DrawHunt(const Device& device, Scheduler& scheduler);
    ~DrawHunt();

    [[nodiscard]] bool Capturing() const noexcept {
        return state == State::Capturing;
    }

    /// After each presented frame, with the interval since the previous one.
    void FrameEnd(double interval_ms);

    /// Draws issued by an indirect call expanded on the CPU (DrawIndirectOnCpu).
    bool cpu_indirect{};

private:
    friend class HuntScope;
    enum class State { Idle, Armed, Capturing, Resolving };

    void Begin();
    void Finish();
    void Write();
    [[nodiscard]] u32 PassOf(const Framebuffer& framebuffer);
    /// A timestamp to measure the next call from, when the list being recorded has none yet.
    void Baseline();
    [[nodiscard]] u32 Stamp();

    const Device& device;
    Scheduler& scheduler;
    State state{State::Idle};
    u32 frames_left{};
    u32 warmup_left{};
    u32 captures{};
    u64 frequency{};
    u64 resolve_tick{};
    u32 resolved{};
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    Hunt::Capture capture;
    VideoCore::Perf::Snapshot perf_start{};
    bool detailed_before{};
    std::chrono::steady_clock::time_point started{};
    bool owns_profiler{};
    std::chrono::steady_clock::time_point profiler_until{};
};

/// One guest draw, clear or dispatch for the hunt; committed when it goes out of scope.
class HuntScope {
public:
    HuntScope(DrawHunt& hunt, Hunt::Kind kind) noexcept
        : hunt{hunt.Capturing() ? &hunt : nullptr} {
        if (this->hunt) [[unlikely]] {
            record.kind = kind;
            record.macro = VideoCore::Perf::current_macro;
            record.flags = hunt.cpu_indirect ? Hunt::CpuIndirect : 0;
            start = std::chrono::steady_clock::now();
        }
    }
    ~HuntScope();
    HuntScope(const HuntScope&) = delete;
    HuntScope& operator=(const HuntScope&) = delete;

    void Outcome(Hunt::Outcome outcome) noexcept {
        if (hunt) [[unlikely]] record.outcome = outcome;
    }
    void Shaders(u64 vs, u64 ps) noexcept {
        if (hunt) [[unlikely]] {
            record.vs = vs;
            record.ps = ps;
        }
    }
    void PipelineStart() noexcept {
        if (hunt) [[unlikely]] pipeline_start = std::chrono::steady_clock::now();
    }
    void PipelineEnd() noexcept;
    void Geometry(u32 count, u32 instances, u32 topology, u8 flags = 0) noexcept {
        if (hunt) [[unlikely]] {
            record.count = count;
            record.instances = instances;
            record.topology = static_cast<u8>(topology);
            record.flags |= flags;
        }
    }
    /// The target the call draws to (none for a dispatch); call before recording it.
    void Target(const Framebuffer* framebuffer);
    /// The call is in the list: timestamps it.
    void Recorded();

private:
    DrawHunt* hunt;
    Hunt::Record record;
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point pipeline_start{};
};

} // namespace D3D12
