// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <fstream>
#include <utility>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/windows/sampling_profiler.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_draw_hunt.h"

namespace D3D12 {

namespace {

using VideoCore::Perf::Counter;

/// About 4 MiB of records and 2 MiB of timestamps, only while a capture runs: far more than
/// eight frames of the heaviest scene seen so far.
constexpr u32 MAX_RECORDS = 1u << 17;
constexpr u32 MAX_TIMESTAMPS = 1u << 18;
/// The CPU profile spans more than the captured frames: a few seconds give stable stacks.
constexpr auto PROFILE_TIME = std::chrono::seconds{5};
constexpr u32 PROFILE_HZ = 500;
/// Frames skipped after the request: closing the game menu hitches the first ones.
constexpr u32 WARMUP_FRAMES = 30;

std::atomic<u32> hunt_requested{};

struct NamedCounter {
    Counter counter;
    const char* name;
};
/// The counters the report reads, as deltas over the captured frames.
constexpr NamedCounter REPORTED[] = {
    {Counter::GpuThreadIdleUs, "GpuThreadIdleUs"},
    {Counter::GpuThreadFlushes, "GpuThreadFlushes"},
    {Counter::GpuThreadFlushUs, "GpuThreadFlushUs"},
    {Counter::Submits, "Submits"},
    {Counter::FenceWaits, "FenceWaits"},
    {Counter::FenceWaitUs, "FenceWaitUs"},
    {Counter::GpuBusyUs, "GpuBusyUs"},
    {Counter::PipelineStalls, "PipelineStalls"},
    {Counter::PipelineStallUs, "PipelineStallUs"},
    {Counter::PsoCreates, "PsoCreates"},
    {Counter::Draws, "Draws"},
    {Counter::Dispatches, "Dispatches"},
    {Counter::TextureUploads, "TextureUploads"},
    {Counter::TextureUploadBytes, "TextureUploadBytes"},
    {Counter::TextureDownloads, "TextureDownloads"},
    {Counter::TextureDownloadBytes, "TextureDownloadBytes"},
    {Counter::ResourcesCreated, "ResourcesCreated"},
    {Counter::ResourceCreateUs, "ResourceCreateUs"},
    {Counter::StagingStreamWaits, "StagingStreamWaits"},
    {Counter::TextureDecodeUs, "TextureDecodeUs"},
    {Counter::DrawNs, "DrawNs"},
    {Counter::DrawTexturesNs, "DrawTexturesNs"},
    {Counter::DrawBuffersNs, "DrawBuffersNs"},
    {Counter::DrawDescriptorsNs, "DrawDescriptorsNs"},
    {Counter::DrawTargetsNs, "DrawTargetsNs"},
    {Counter::DrawSamplersNs, "DrawSamplersNs"},
    {Counter::DrawRecordNs, "DrawRecordNs"},
    {Counter::ClearNs, "ClearNs"},
    {Counter::DispatchNs, "DispatchNs"},
    {Counter::CacheInvalidationNs, "CacheInvalidationNs"},
    {Counter::DmaPullerNs, "DmaPullerNs"},
    {Counter::DmaMacroNs, "DmaMacroNs"},
    {Counter::DmaMaxwellNs, "DmaMaxwellNs"},
    {Counter::DmaComputeNs, "DmaComputeNs"},
    {Counter::DmaCopyNs, "DmaCopyNs"},
    {Counter::GuestGpuWaits, "GuestGpuWaits"},
    {Counter::GuestGpuWaitUs, "GuestGpuWaitUs"},
    {Counter::GuestCoreIdleUs, "GuestCoreIdleUs"},
    {Counter::GuestFramesQueued, "GuestFramesQueued"},
    {Counter::Composites, "Composites"},
};

u32 ElapsedNs(std::chrono::steady_clock::time_point since) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - since)
                        .count();
    return static_cast<u32>(std::clamp<long long>(ns, 0, 0xFFFFFFFF));
}

} // Anonymous namespace

void HuntNextFrames(u32 frames) {
    hunt_requested.store(std::clamp<u32>(frames, 1, 120), std::memory_order_relaxed);
}

DrawHunt::DrawHunt(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {}

DrawHunt::~DrawHunt() {
    if (owns_profiler) {
        Common::SamplingProfiler::Stop();
    }
}

void DrawHunt::Begin() {
    if (!heap) {
        if (FAILED(device.Queue()->GetTimestampFrequency(&frequency)) || frequency == 0) {
            LOG_WARNING(Render, "D3D12 hunt: no queue timestamps, GPU times stay unknown");
            frequency = 0;
        }
        ID3D12Device* const dev = device.Get();
        const D3D12_QUERY_HEAP_DESC heap_desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                              .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        const D3D12_HEAP_PROPERTIES heap_properties{.Type = D3D12_HEAP_TYPE_READBACK};
        const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Alignment = 0, .Width = MAX_TIMESTAMPS * sizeof(u64), .Height = 1,
            .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = {.Count = 1, .Quality = 0}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = D3D12_RESOURCE_FLAG_NONE};
        if (frequency != 0 &&
            (FAILED(dev->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap))) ||
             FAILED(dev->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&readback))))) {
            LOG_WARNING(Render, "D3D12 hunt: timestamp queries unavailable, GPU times stay unknown");
            heap.Reset();
            readback.Reset();
        }
    }
    capture.Reset(MAX_RECORDS, heap ? MAX_TIMESTAMPS : 0);
    // The per-stage draw counters only run with the detailed profile; the capture wants them.
    detailed_before = VideoCore::Perf::DetailedGpuProfileEnabled();
    VideoCore::Perf::SetDetailedGpuProfile(true);
    perf_start = VideoCore::Perf::Read();
    started = std::chrono::steady_clock::now();
    if (!Common::SamplingProfiler::Running()) {
        Common::SamplingProfiler::Start(
            {.hz = PROFILE_HZ,
             .window_seconds = static_cast<u32>(PROFILE_TIME.count()),
             .output = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                       "jadyx_hunt_profile.txt"});
        owns_profiler = true;
        profiler_until = started + PROFILE_TIME;
    }
}

void DrawHunt::FrameEnd(double interval_ms) {
    if (owns_profiler && std::chrono::steady_clock::now() >= profiler_until) {
        Common::SamplingProfiler::Stop();
        owns_profiler = false;
        LOG_INFO(Render, "D3D12 hunt: CPU profile written (jadyx_hunt_profile.txt)");
    }
    switch (state) {
    case State::Idle:
        if (const u32 frames = hunt_requested.exchange(0, std::memory_order_relaxed)) {
            frames_left = frames;
            warmup_left = WARMUP_FRAMES;
            state = State::Armed;
        }
        break;
    case State::Armed:
        if (--warmup_left == 0) {
            Begin();
            state = State::Capturing;
            LOG_INFO(Render, "D3D12 hunt: capturing {} frames", frames_left);
        }
        break;
    case State::Capturing:
        capture.EndFrame(interval_ms);
        if (--frames_left == 0) {
            Finish();
        }
        break;
    case State::Resolving:
        if (scheduler.IsFree(resolve_tick)) {
            Write();
            state = State::Idle;
        }
        break;
    }
}

void DrawHunt::Finish() {
    resolved = capture.Timestamps();
    if (resolved != 0) {
        scheduler.CommandList()->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                                  resolved, readback.Get(), 0);
    }
    resolve_tick = scheduler.CurrentTick();
    state = State::Resolving;
}

void DrawHunt::Write() {
    const VideoCore::Perf::Snapshot perf_end = VideoCore::Perf::Read();
    VideoCore::Perf::SetDetailedGpuProfile(detailed_before);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    // Counters run until the timestamps are back, a frame or two past the capture: the report
    // divides them by this time, not by the captured frames.
    std::string extra = fmt::format("meta frequency {} counters_ms {:.3f}\n", frequency, elapsed_ms);
    for (const NamedCounter& named : REPORTED) {
        extra += fmt::format("perf {} {}\n", named.name,
                             VideoCore::Perf::Get(perf_end, named.counter) -
                                 VideoCore::Perf::Get(perf_start, named.counter));
    }
    const u64* values = nullptr;
    const D3D12_RANGE range{0, resolved * sizeof(u64)};
    void* mapped = nullptr;
    if (resolved != 0 && SUCCEEDED(readback->Map(0, &range, &mapped))) {
        values = static_cast<const u64*>(mapped);
    }
    const std::string text = capture.Serialize(
        ++captures, values ? std::span<const u64>{values, resolved} : std::span<const u64>{},
        frequency, extra);
    if (mapped) {
        const D3D12_RANGE nothing{0, 0};
        readback->Unmap(0, &nothing);
    }
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "jadyx_hunt.txt";
    // The first capture of a session replaces the previous session's file.
    std::ofstream file{path, captures == 1 ? std::ios::trunc : std::ios::app};
    file << text;
    LOG_INFO(Render, "D3D12 hunt: capture {} written ({} records, {} frames) to jadyx_hunt.txt",
             captures, capture.Records().size(), capture.Frame());
    capture = {}; // returns the record memory
}

u32 DrawHunt::PassOf(const Framebuffer& framebuffer) {
    const VideoCommon::Extent2D extent = framebuffer.Extent();
    const auto colors = static_cast<u32>(framebuffer.ColorTargets().size());
    return capture.InternPass({
        .id = &framebuffer,
        .width = extent.width,
        .height = extent.height,
        .samples = framebuffer.Samples(),
        .colors = colors,
        .color_format = colors != 0 ? static_cast<u32>(framebuffer.ColorFormat(0)) : 0,
        .depth_format = framebuffer.DepthTarget().ptr != 0
                            ? static_cast<u32>(framebuffer.DepthFormat())
                            : 0,
    });
}

void DrawHunt::Baseline() {
    const u64 tick = scheduler.CurrentTick();
    if (!heap || capture.HasBaseline(tick)) {
        return;
    }
    if (const std::optional<u32> index = capture.TakeTimestamp(tick)) {
        scheduler.CommandList()->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, *index);
    }
}

u32 DrawHunt::Stamp() {
    if (!heap) {
        return Hunt::NoTimestamp;
    }
    // When a flush came between Baseline and here, this stamp only starts the new list: the
    // capture sees the ticks differ and leaves this call's GPU time unknown.
    const std::optional<u32> index = capture.TakeTimestamp(scheduler.CurrentTick());
    if (!index) {
        return Hunt::NoTimestamp;
    }
    scheduler.CommandList()->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, *index);
    return *index;
}

HuntScope::~HuntScope() {
    if (!hunt) [[likely]] {
        return;
    }
    record.cpu_ns = ElapsedNs(start);
    record.frame = hunt->capture.Frame();
    hunt->capture.Add(record);
}

void HuntScope::PipelineEnd() noexcept {
    if (hunt) [[unlikely]] {
        record.pipeline_ns = ElapsedNs(pipeline_start);
    }
}

void HuntScope::Target(const Framebuffer* framebuffer) {
    if (!hunt) [[likely]] {
        return;
    }
    if (framebuffer) {
        record.pass = hunt->PassOf(*framebuffer);
    }
    hunt->Baseline();
}

void HuntScope::Recorded() {
    if (!hunt) [[likely]] {
        return;
    }
    record.outcome = Hunt::Outcome::Recorded;
    record.timestamp = hunt->Stamp();
}

} // namespace D3D12
