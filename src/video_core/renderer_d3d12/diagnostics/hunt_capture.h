// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

/// Bottleneck hunt: what every draw, clear and dispatch of a few frames cost, and why. One record
/// per guest call (recorded or skipped) with its shaders, target, geometry, CPU time and the GPU
/// time between its timestamp and the previous one in the same command list. The PC side
/// (tools/xbox/hunt-report.py) ranks passes, pipelines, macros and batchable runs.
/// Pure bookkeeping, so it is tested on the PC; d3d12_draw_hunt.cpp owns the D3D12 objects.
namespace D3D12::Hunt {

enum class Kind : u8 { Draw, DrawIndexed, DrawIndirect, DrawTexture, Clear, Dispatch };

enum class Outcome : u8 {
    Recorded,
    Skipped,       ///< any other early return (integer formats, empty clear, group limits)
    Conditional,   ///< conditional rendering said no
    Culled,        ///< both faces culled
    NoPipeline,
    PsoRejected,
    SkippedShader, ///< boot.cfg skip_shader
    Empty,         ///< zero vertices or instances
    NoTarget,
    Expanded,      ///< an indirect call split into CPU draws, which follow as records of their own
};

enum Flags : u8 {
    CpuIndirect = 1,      ///< one of the draws an indirect call became on the CPU
    EmulatedTopology = 2, ///< quads/loops/fans rebuilt into an index list on the CPU
};

inline constexpr u32 NoTimestamp = 0xFFFFFFFF;
inline constexpr u32 NoPass = 0xFFFFFFFF;

struct Pass {
    const void* id{}; ///< the framebuffer object; equal targets in different objects stay apart
    u32 width{};
    u32 height{};
    u32 samples{};
    u32 colors{};
    u32 color_format{}; ///< DXGI_FORMAT of attachment 0, 0 without color
    u32 depth_format{};
    bool operator==(const Pass&) const = default;
};

struct Record {
    u64 vs{};
    u64 ps{};       ///< or the compute shader of a dispatch
    u32 pass{NoPass};
    u32 count{};    ///< vertices or indices; draws of an ExecuteIndirect; groups of a dispatch
    u32 instances{};
    u32 cpu_ns{};   ///< the whole call on the GPU thread, pipeline wait included
    u32 pipeline_ns{};
    u32 macro{};    ///< guest macro method that issued the call, 0 when none
    u32 timestamp{NoTimestamp};
    u32 frame{};
    u8 topology{};
    Kind kind{};
    Outcome outcome{Outcome::Skipped};
    u8 flags{};
};

class Capture {
public:
    void Reset(u32 record_capacity, u32 timestamp_capacity) {
        records.clear();
        passes.clear();
        frames_ms.clear();
        timestamp_ticks.clear();
        records.reserve(record_capacity);
        max_records = record_capacity;
        max_timestamps = timestamp_capacity;
        dropped = 0;
    }

    [[nodiscard]] u32 InternPass(const Pass& pass) {
        // Few targets per frame; the last one is the common hit.
        if (!passes.empty() && passes.back() == pass) return static_cast<u32>(passes.size() - 1);
        for (size_t i = 0; i < passes.size(); ++i)
            if (passes[i] == pass) return static_cast<u32>(i);
        passes.push_back(pass);
        return static_cast<u32>(passes.size() - 1);
    }

    /// Index for a new timestamp recorded in the list that signals tick, or none when full.
    [[nodiscard]] std::optional<u32> TakeTimestamp(u64 tick) {
        if (timestamp_ticks.size() >= max_timestamps) return std::nullopt;
        timestamp_ticks.push_back(tick);
        return static_cast<u32>(timestamp_ticks.size() - 1);
    }

    /// Whether the newest timestamp belongs to tick's list (a draw can measure from it).
    [[nodiscard]] bool HasBaseline(u64 tick) const {
        return !timestamp_ticks.empty() && timestamp_ticks.back() == tick;
    }

    [[nodiscard]] bool Full() const { return records.size() >= max_records; }

    void Add(const Record& record) {
        if (Full()) {
            ++dropped;
            return;
        }
        records.push_back(record);
    }

    void EndFrame(double interval_ms) { frames_ms.push_back(interval_ms); }

    [[nodiscard]] u32 Frame() const { return static_cast<u32>(frames_ms.size()); }
    [[nodiscard]] u32 Timestamps() const { return static_cast<u32>(timestamp_ticks.size()); }
    [[nodiscard]] std::span<const Record> Records() const { return records; }
    [[nodiscard]] std::span<const Pass> Passes() const { return passes; }

    /// GPU nanoseconds from the previous timestamp of the same list to the record's, when both
    /// exist and the clock moved forward.
    [[nodiscard]] std::optional<u64> GpuNs(const Record& record, std::span<const u64> values,
                                           u64 frequency) const {
        const u32 index = record.timestamp;
        if (index == NoTimestamp || index == 0 || index >= values.size() ||
            index >= timestamp_ticks.size() || frequency == 0)
            return std::nullopt;
        if (timestamp_ticks[index - 1] != timestamp_ticks[index]) return std::nullopt;
        if (values[index] < values[index - 1]) return std::nullopt;
        const u64 ticks = values[index] - values[index - 1];
        return static_cast<u64>(static_cast<double>(ticks) * 1e9 / static_cast<double>(frequency));
    }

    /// Text read by tools/xbox/hunt-report.py. "-" stands for an unknown GPU time.
    [[nodiscard]] std::string Serialize(u32 number, std::span<const u64> values, u64 frequency,
                                        std::string_view extra = {}) const {
        std::string out;
        out.reserve(64 + records.size() * 96);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "hunt 1 capture %u frames %zu records %zu dropped %u timestamps %zu\n",
                      number, frames_ms.size(), records.size(), dropped, timestamp_ticks.size());
        out += line;
        for (size_t i = 0; i < frames_ms.size(); ++i) {
            std::snprintf(line, sizeof(line), "frame %zu %.3f\n", i, frames_ms[i]);
            out += line;
        }
        for (size_t i = 0; i < passes.size(); ++i) {
            const Pass& p = passes[i];
            std::snprintf(line, sizeof(line), "pass %zu %ux%u s%u c%u f%u d%u\n", i, p.width,
                          p.height, p.samples, p.colors, p.color_format, p.depth_format);
            out += line;
        }
        out += extra;
        for (const Record& r : records) {
            const std::optional<u64> gpu = GpuNs(r, values, frequency);
            char gpu_text[24] = "-";
            if (gpu) std::snprintf(gpu_text, sizeof(gpu_text), "%llu",
                                   static_cast<unsigned long long>(*gpu));
            std::snprintf(line, sizeof(line),
                          "r %u %u %u %u %d %016llx %016llx %u %u %u %u %u %s %x\n",
                          static_cast<u32>(r.kind), static_cast<u32>(r.outcome),
                          static_cast<u32>(r.flags), r.frame,
                          r.pass == NoPass ? -1 : static_cast<int>(r.pass),
                          static_cast<unsigned long long>(r.vs),
                          static_cast<unsigned long long>(r.ps), r.count, r.instances,
                          static_cast<u32>(r.topology), r.cpu_ns, r.pipeline_ns, gpu_text,
                          r.macro);
            out += line;
        }
        out += "end\n";
        return out;
    }

private:
    std::vector<Record> records;
    std::vector<Pass> passes;
    std::vector<double> frames_ms;
    std::vector<u64> timestamp_ticks; ///< the scheduler tick of each timestamp's list
    u32 max_records{};
    u32 max_timestamps{};
    u32 dropped{};
};

} // namespace D3D12::Hunt
