// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/arm/cpu_profile.h"
#include "dynarmic/interface/jit_profile.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"

// Frame pacing statistics and the periodic performance reports in the log.

namespace D3D12 {

void RendererD3D12::RecordPacing(double wait_ms, double present_ms) {
    device.LogDebugMessages();
    constexpr u32 PACING_WINDOW = PacingStats::WINDOW;
    constexpr double HITCH_MS = 1000.0 / 60.0 * 1.5;
    const auto now = std::chrono::steady_clock::now();
    if (pacing.last_composite != std::chrono::steady_clock::time_point{}) {
        const double interval =
            std::chrono::duration<double, std::milli>(now - pacing.last_composite).count();
        pacing.intervals_ms[pacing.frames++] = interval;
        ++performance_overlay.frames;
        performance_overlay.last_frame_ms = interval;
        performance_overlay.max_frame_ms = std::max(performance_overlay.max_frame_ms, interval);
        pacing.total_ms += interval;
        pacing.max_interval_ms = std::max(pacing.max_interval_ms, interval);
        pacing.hitches += interval > HITCH_MS ? 1 : 0;
        pacing.max_wait_ms = std::max(pacing.max_wait_ms, wait_ms);
        pacing.max_present_ms = std::max(pacing.max_present_ms, present_ms);
        ReportPerf(interval);
    } else {
        perf_frame = VideoCore::Perf::Read();
        perf_window = perf_frame;
    }
    pacing.last_composite = now;
    if (pacing.frames == PACING_WINDOW) {
        // Nearest-rank percentiles over presented intervals. Sort once per window, with no
        // allocation or extra clocks per frame. p99 exposes hitches hidden by the mean.
        auto sorted = pacing.intervals_ms;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](u32 percent) {
            return sorted[(PACING_WINDOW * percent + 99) / 100 - 1];
        };
        LOG_INFO(Render,
                 "D3D12 pacing: {} frames, avg {:.2f} ms, max {:.2f} ms, {} hitches; max wait "
                 "{:.2f} ms, max record+present {:.2f} ms",
                 pacing.frames, pacing.total_ms / pacing.frames, pacing.max_interval_ms,
                 pacing.hitches, pacing.max_wait_ms, pacing.max_present_ms);
        LOG_INFO(Render, "D3D12 frame times: {:.2f} FPS, p50 {:.2f} ms, p95 {:.2f} ms, "
                         "p99 {:.2f} ms ({} presented intervals)",
                 1000.0 * pacing.frames / pacing.total_ms, percentile(50), percentile(95),
                 percentile(99), pacing.frames);
        ReportPerfWindow(pacing.frames, pacing.total_ms);
        pacing = PacingStats{.last_composite = now};
    }
}

namespace {

using VideoCore::Perf::Counter;

/// The change of every counter between two snapshots.
VideoCore::Perf::Snapshot Delta(const VideoCore::Perf::Snapshot& now,
                                const VideoCore::Perf::Snapshot& before) {
    VideoCore::Perf::Snapshot delta{};
    for (size_t i = 0; i < delta.size(); ++i) {
        delta[i] = now[i] - before[i];
    }
    return delta;
}

double Ms(const VideoCore::Perf::Snapshot& delta, Counter counter) {
    return static_cast<double>(VideoCore::Perf::Get(delta, counter)) / 1000.0;
}

/// Counters of one frame or window, one line. The GPU thread's time splits into idle (waiting
/// for the guest), fence waits (waiting for the GPU), pipeline stalls, guest flushes, resource
/// creation, and the rest: recording draws and running the caches.
std::string DescribePerf(const VideoCore::Perf::Snapshot& d, double interval_ms) {
    const double idle = Ms(d, Counter::GpuThreadIdleUs);
    const double fence = Ms(d, Counter::FenceWaitUs);
    const double pipelines = Ms(d, Counter::PipelineStallUs);
    const double flushes = Ms(d, Counter::GpuThreadFlushUs);
    const double creates = Ms(d, Counter::ResourceCreateUs);
    const double decodes = Ms(d, Counter::TextureDecodeUs);
    const double work =
        std::max(0.0, interval_ms - idle - fence - pipelines - flushes - creates - decodes);
    const auto get = [&d](Counter counter) { return VideoCore::Perf::Get(d, counter); };
    return fmt::format(
        "GPU thread: idle {:.1f} ms, fence waits {} ({:.1f} ms), pipeline stalls {} ({:.1f} ms), "
        "guest flushes {} ({:.1f} ms), new resources {} ({:.1f} ms), CPU texture decode {:.1f} ms, "
        "other work {:.1f} ms | GPU busy {:.1f} ms, {} submits | {} draws, {} dispatches | "
        "uploads {} ({:.2f} MiB, {} GPU-decoded), downloads {} ({:.2f} MiB) | staging: {} "
        "dedicated ({:.2f} MiB), {} ring waits",
        idle, get(Counter::FenceWaits), fence, get(Counter::PipelineStalls), pipelines,
        get(Counter::GpuThreadFlushes), flushes, get(Counter::ResourcesCreated), creates, decodes,
        work,
        Ms(d, Counter::GpuBusyUs), get(Counter::Submits), get(Counter::Draws),
        get(Counter::Dispatches), get(Counter::TextureUploads),
        static_cast<double>(get(Counter::TextureUploadBytes)) / (1024.0 * 1024.0),
        get(Counter::TextureGpuDecodes), get(Counter::TextureDownloads),
        static_cast<double>(get(Counter::TextureDownloadBytes)) / (1024.0 * 1024.0),
        get(Counter::StagingDedicated),
        static_cast<double>(get(Counter::StagingDedicatedBytes)) / (1024.0 * 1024.0),
        get(Counter::StagingStreamWaits));
}

/// Where the GPU thread's draws spend their time (per draw, in microseconds), what is left outside
/// draws, clears and dispatches (Maxwell methods, macros, DMA, presenting), and how long the guest
/// waited for the GPU to signal its fences.
std::string DescribeDrawCosts(const VideoCore::Perf::Snapshot& d, double interval_ms) {
    if (!VideoCore::Perf::DetailedGpuProfileEnabled()) {
        return "detailed per-draw profiling off (boot.cfg gpu_profile=1 enables it)";
    }
    const auto get = [&d](Counter counter) { return VideoCore::Perf::Get(d, counter); };
    const u64 draws = std::max<u64>(1, get(Counter::Draws));
    const auto per_draw = [&](Counter counter) {
        return static_cast<double>(get(counter)) / 1000.0 / static_cast<double>(draws);
    };
    const auto ns_ms = [&](Counter counter) { return static_cast<double>(get(counter)) / 1e6; };
    const double draw_ms = ns_ms(Counter::DrawNs);
    const double clear_ms = ns_ms(Counter::ClearNs);
    const double dispatch_ms = ns_ms(Counter::DispatchNs);
    const double submit_ms = ns_ms(Counter::SubmitListNs);
    const double view_total_ms = ns_ms(Counter::TextureCacheViewCreateNs);
    const double view_setup_ms = ns_ms(Counter::TextureCacheViewSetupNs);
    const double view_reinterpret_ms = ns_ms(Counter::TextureCacheViewReinterpretNs);
    const double view_srv_ms = ns_ms(Counter::TextureCacheViewSrvNs);
    const double view_attachment_ms = ns_ms(Counter::TextureCacheViewAttachmentNs);
    const double view_slot_insert_ms = ns_ms(Counter::TextureCacheViewSlotInsertNs);
    const double view_slot_grow_ms = ns_ms(Counter::TextureCacheViewSlotGrowNs);
    const double view_slot_clock_ms = ns_ms(Counter::TextureCacheViewSlotClockNs);
    const double view_slot_free_ms = ns_ms(Counter::TextureCacheViewSlotFreeNs);
    const double view_slot_construct_ms = ns_ms(Counter::TextureCacheViewSlotConstructNs);
    const double view_slot_bit_ms = ns_ms(Counter::TextureCacheViewSlotBitNs);
    const double view_index_ms = ns_ms(Counter::TextureCacheViewIndexNs);
    const double view_base_ms = ns_ms(Counter::TextureCacheViewBaseNs);
    const double view_compatibility_ms = ns_ms(Counter::TextureCacheViewCompatibilityNs);
    const double view_placement_ms = std::max(
        0.0, view_slot_insert_ms + view_slot_grow_ms - view_base_ms - view_setup_ms -
                 view_reinterpret_ms - view_srv_ms - view_attachment_ms);
    const double submit_overhead = std::max(0.0, submit_ms - draw_ms - clear_ms - dispatch_ms);
    const double outside = std::max(0.0, interval_ms - Ms(d, Counter::GpuThreadIdleUs) -
                                             Ms(d, Counter::FenceWaitUs) -
                                             Ms(d, Counter::GpuThreadFlushUs) - draw_ms -
                                             clear_ms - dispatch_ms);
    return fmt::format(
        "draws {:.1f} ms ({:.1f} us each: textures {:.1f}, buffers {:.1f}, descriptors {:.1f}, "
        "targets {:.1f} [update {:.1f}, feedback {:.1f}, framebuffer {:.1f}, transitions {:.1f}], "
        "samplers {:.1f}, record {:.1f}), clears {:.1f} ms, dispatches {:.1f} ms, "
        "outside them {:.1f} ms | pipeline fast path {} hits / {} misses | CBV {} ({:.1f} ms: "
        "{} streamed, {} persistent, {} null), view copies {} ({:.1f} ms) | guest waited for "
        "the GPU {} times ({:.1f} ms in all) | commands: {} submit lists {:.1f} ms ({:.1f} ms "
        "outside draw/clear/dispatch), {} ticks {:.1f} ms, {} invalidations {:.1f} ms | uploads: "
        "{} maps {:.1f} ms, {} repacks {:.1f} ms, {} copies {:.1f} ms | texture cache: "
        "{} finds {:.1f} ms, {} inserts {:.1f} ms (overlap {:.1f}, image {:.1f}, refresh "
        "{:.1f}, register {:.1f}), {} views {:.1f} ms [setup {:.1f}, reinterpret {:.1f}, "
        "base {:.1f} (compat {:.1f}), SRV {:.1f}, attachments {:.1f}, slot {:.1f}, "
        "grow {} / {:.1f}, placement {:.1f} [clock {:.1f}, free {:.1f}, construct {:.1f}, bit "
        "{:.1f}], index {:.1f}], "
        "staging {:.1f}, unswizzle {:.1f}, "
        "backend {:.1f} ms | DMA: "
        "puller {} / {:.1f} ms, macros {} / {:.1f} ms, Maxwell {} / {:.1f} ms, compute {} / "
        "{:.1f} ms, copies {} / {:.1f} ms, other {} / {:.1f} ms | Maxwell dirty: {} changed, "
        "{} identical skipped",
        draw_ms, per_draw(Counter::DrawNs), per_draw(Counter::DrawTexturesNs),
        per_draw(Counter::DrawBuffersNs), per_draw(Counter::DrawDescriptorsNs),
        per_draw(Counter::DrawTargetsNs), per_draw(Counter::DrawUpdateTargetsNs),
        per_draw(Counter::DrawFeedbackNs), per_draw(Counter::DrawFramebufferNs),
        per_draw(Counter::DrawImageTransitionsNs), per_draw(Counter::DrawSamplersNs),
        per_draw(Counter::DrawRecordNs), clear_ms, dispatch_ms, outside,
        get(Counter::PipelineFastHits), get(Counter::PipelineFastMisses),
        get(Counter::CbvCreates), ns_ms(Counter::CbvCreateNs), get(Counter::CbvStreamed),
        get(Counter::CbvPersistent), get(Counter::CbvNull), get(Counter::ViewCopies),
        ns_ms(Counter::ViewCopyNs),
        get(Counter::GuestGpuWaits), Ms(d, Counter::GuestGpuWaitUs), get(Counter::SubmitLists),
        submit_ms, submit_overhead, get(Counter::GpuTicks), ns_ms(Counter::GpuTickNs),
        get(Counter::CacheInvalidations), ns_ms(Counter::CacheInvalidationNs),
        get(Counter::TextureUploadMaps), ns_ms(Counter::TextureUploadMapNs),
        get(Counter::TextureUploadRepacks), ns_ms(Counter::TextureUploadRepackNs),
        get(Counter::TextureUploadCopies), ns_ms(Counter::TextureUploadRecordNs),
        get(Counter::TextureCacheFinds), ns_ms(Counter::TextureCacheFindNs),
        get(Counter::TextureCacheInserts), ns_ms(Counter::TextureCacheInsertNs),
        ns_ms(Counter::TextureCacheOverlapNs), ns_ms(Counter::TextureCacheImageCreateNs),
        ns_ms(Counter::TextureCacheRefreshNs), ns_ms(Counter::TextureCacheRegisterNs),
        get(Counter::TextureCacheViewsCreated), view_total_ms, view_setup_ms, view_reinterpret_ms,
        view_base_ms, view_compatibility_ms, view_srv_ms, view_attachment_ms, view_slot_insert_ms,
        get(Counter::TextureCacheViewSlotGrows), view_slot_grow_ms, view_placement_ms,
        view_slot_clock_ms, view_slot_free_ms, view_slot_construct_ms, view_slot_bit_ms,
        view_index_ms,
        ns_ms(Counter::TextureCacheStagingNs), ns_ms(Counter::TextureCacheUnswizzleNs),
        ns_ms(Counter::TextureCacheBackendUploadNs),
        get(Counter::DmaPullerCalls), ns_ms(Counter::DmaPullerNs), get(Counter::DmaMacroCalls),
        ns_ms(Counter::DmaMacroNs), get(Counter::DmaMaxwellCalls), ns_ms(Counter::DmaMaxwellNs),
        get(Counter::DmaComputeCalls), ns_ms(Counter::DmaComputeNs), get(Counter::DmaCopyCalls),
        ns_ms(Counter::DmaCopyNs), get(Counter::DmaOtherCalls), ns_ms(Counter::DmaOtherNs),
        get(Counter::MaxwellDirtyChanged), get(Counter::MaxwellDirtyUnchanged));
}

std::string DescribeGuestWaitSites() {
    const auto sites = VideoCore::Perf::TakeGuestWaitSites();
    std::array<size_t, VideoCore::Perf::NUM_GUEST_SYNCPOINTS> order{};
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&sites](size_t left, size_t right) {
        return sites[left].total_us > sites[right].total_us;
    });
    std::string result;
    size_t emitted{};
    for (const size_t id : order) {
        if (sites[id].count == 0 || emitted == 6) {
            break;
        }
        if (!result.empty()) {
            result += "; ";
        }
        result += fmt::format("id {}: {} waits, {:.1f} ms total, {:.1f} ms max", id,
                              sites[id].count, static_cast<double>(sites[id].total_us) / 1000.0,
                              static_cast<double>(sites[id].max_us) / 1000.0);
        ++emitted;
    }
    return result.empty() ? "none" : result;
}

std::string DescribeMacroProfiles() {
    auto profiles = VideoCore::Perf::TakeMacroProfiles();
    std::ranges::sort(profiles, [](const auto& left, const auto& right) {
        return left.total_ns - std::min(left.total_ns, left.nested_ns) >
               right.total_ns - std::min(right.total_ns, right.nested_ns);
    });
    std::string result;
    for (size_t index = 0; index < std::min<size_t>(profiles.size(), 6); ++index) {
        const auto& profile = profiles[index];
        const double exclusive_ms =
            static_cast<double>(profile.total_ns - std::min(profile.total_ns, profile.nested_ns)) /
            1e6;
        if (!result.empty()) {
            result += "; ";
        }
        result += fmt::format("m=0x{:x} h={:016x}: {} calls, {:.1f} ms exclusive, {:.1f} ms "
                              "nested, {:.2f} ms max",
                              profile.method, profile.hash, profile.count, exclusive_ms,
                              static_cast<double>(profile.nested_ns) / 1e6,
                              static_cast<double>(profile.max_ns) / 1e6);
    }
    return result.empty() ? "none" : result;
}

/// The frame chain over a window: frames the game queued, vsyncs (and those lost to a late one),
/// the VSyncThread waiting for the GPU thread, how long composites took to run after being
/// requested, and the game waiting for a free framebuffer.
std::string DescribeFrameChain(const VideoCore::Perf::Snapshot& d) {
    const auto get = [&d](Counter counter) { return VideoCore::Perf::Get(d, counter); };
    const auto average_ms = [&](Counter us, Counter count) {
        return get(count) != 0 ? Ms(d, us) / static_cast<double>(get(count)) : 0.0;
    };
    return fmt::format(
        "game queued {} frames | vsyncs {} ({} lost), {} with a new frame; waited for the GPU "
        "thread {:.1f} ms | composites {} ({:.1f} ms after the request on average, {} over a "
        "frame) | game waited for a free framebuffer {} times ({:.1f} ms) | forced {} swap "
        "intervals to one | emulated cores idle {:.1f} ms in all",
        get(Counter::GuestFramesQueued), get(Counter::Vsyncs), get(Counter::VsyncsLost),
        get(Counter::VsyncFrames), Ms(d, Counter::VsyncComposeWaitUs), get(Counter::Composites),
        average_ms(Counter::CompositeLatencyUs, Counter::Composites), get(Counter::CompositesLate),
        get(Counter::GuestDequeueWaits), Ms(d, Counter::GuestDequeueWaitUs),
        get(Counter::GuestSwapIntervalOverrides),
        Ms(d, Counter::GuestCoreIdleUs));
}

/// The largest share of a slow frame, in words.
const char* LikelyCause(const VideoCore::Perf::Snapshot& d, double interval_ms) {
    const std::array<std::pair<double, const char*>, 7> shares{{
        {Ms(d, Counter::GpuThreadIdleUs), "guest CPU (the GPU thread waited for work)"},
        {Ms(d, Counter::FenceWaitUs), "GPU (the GPU thread waited for the GPU)"},
        {Ms(d, Counter::PipelineStallUs), "pipeline builds"},
        {Ms(d, Counter::GpuThreadFlushUs), "guest reading back GPU memory"},
        {Ms(d, Counter::ResourceCreateUs), "creating resources"},
        {Ms(d, Counter::TextureDecodeUs), "decoding textures on the CPU"},
        {0.0, "recording draws / texture and buffer caches"},
    }};
    double accounted = 0.0;
    for (const auto& share : shares) {
        accounted += share.first;
    }
    const double other = std::max(0.0, interval_ms - accounted);
    const auto* best = &shares.back();
    double best_ms = other;
    for (const auto& share : shares) {
        if (share.first > best_ms) {
            best_ms = share.first;
            best = &share;
        }
    }
    return best->second;
}

} // Anonymous namespace

void RendererD3D12::ReportPerf(double interval_ms) {
    constexpr double REPORT_MS = 100.0;
    const VideoCore::Perf::Snapshot now = VideoCore::Perf::Read();
    const VideoCore::Perf::Snapshot delta = Delta(now, perf_frame);
    perf_frame = now;
    const auto time = std::chrono::steady_clock::now();
    // At most one per second, so a stretch of slow frames does not flood the log.
    if (interval_ms < REPORT_MS || time - last_hitch_report < std::chrono::seconds{1}) {
        return;
    }
    last_hitch_report = time;
    LOG_INFO(Render, "D3D12 hitch: {:.0f} ms frame, likely {} | {}", interval_ms,
             LikelyCause(delta, interval_ms), DescribePerf(delta, interval_ms));
    LOG_INFO(Render, "D3D12 hitch detail: {}", DescribeDrawCosts(delta, interval_ms));
}

void RendererD3D12::ReportPerfWindow(u32 frames, double total_ms) {
    const VideoCore::Perf::Snapshot now = VideoCore::Perf::Read();
    const VideoCore::Perf::Snapshot delta = Delta(now, perf_window);
    perf_window = now;
    LOG_INFO(Render, "D3D12 perf over {} frames ({:.0f} ms), mostly {} | {}", frames, total_ms,
             LikelyCause(delta, total_ms), DescribePerf(delta, total_ms));
    LOG_INFO(Render, "D3D12 GPU thread: {}", DescribeDrawCosts(delta, total_ms));
    LOG_INFO(Render, "D3D12 frame chain: {}", DescribeFrameChain(delta));
    LOG_INFO(Render, "D3D12 depth feedback: {} GPU copies in total",
             texture_cache_runtime.DepthFeedbackCopies());
    if (Core::CpuProfile::Enabled()) {
        const auto jit = Dynarmic::JitProfile::Take();
        const auto measurement = [&](Dynarmic::JitProfile::Phase phase) -> const auto& {
            return jit[static_cast<size_t>(phase)];
        };
        using Phase = Dynarmic::JitProfile::Phase;
        LOG_INFO(Render, "D3D12 guest JIT compilation: {} blocks, {:.1f} ms total; "
                         "translate {:.1f} ms, optimize {:.1f} ms, emit {:.1f} ms; "
                         "{} page protection calls, {:.1f} ms; {} invalidations, {:.1f} ms "
                         "(elapsed across cores; phases/protection overlap)",
                 measurement(Phase::Compile).calls, measurement(Phase::Compile).ns / 1.0e6,
                 measurement(Phase::Translate).ns / 1.0e6,
                 measurement(Phase::Optimize).ns / 1.0e6,
                 measurement(Phase::Emit).ns / 1.0e6,
                 measurement(Phase::Protect).calls, measurement(Phase::Protect).ns / 1.0e6,
                 measurement(Phase::Invalidate).calls, measurement(Phase::Invalidate).ns / 1.0e6);
        for (size_t i = static_cast<size_t>(Phase::WriteOpen); i < jit.size(); ++i) {
            LOG_INFO(Render, "D3D12 JIT emission {}: {} calls, {:.3f} ms (nested elapsed)",
                     Dynarmic::JitProfile::names[i], jit[i].calls, jit[i].ns / 1.0e6);
        }
        using Core::CpuProfile::Counter;
        for (size_t core = 0; core < Core::CpuProfile::cores.size(); ++core) {
            const auto cpu = Core::CpuProfile::Take(core);
            LOG_INFO(Render, "D3D12 guest CPU core {}: {} JIT runs, {:.1f} ms elapsed in Run "
                             "(includes translation/callbacks/preemption), {} code words, "
                             "{} slow reads ({} vectors), {} slow writes, {} clock reads",
                     core, Get(cpu, Counter::Runs), Get(cpu, Counter::RunNs) / 1.0e6,
                     Get(cpu, Counter::CodeWords), Get(cpu, Counter::Reads),
                     Get(cpu, Counter::Reads128),
                     Get(cpu, Counter::Writes), Get(cpu, Counter::ClockReads));
            const u64 samples = Get(cpu, Counter::ReadSamples);
            LOG_INFO(Render, "D3D12 guest CPU core {} callbacks: {} read samples, {:.0f} ns "
                             "mean sampled read; {} flush-area misses, {:.1f} ms "
                             "(cache locks and possible GPU downloads included)",
                     core, samples, samples ? static_cast<double>(Get(cpu, Counter::ReadSampleNs)) /
                                                 samples : 0.0,
                     Get(cpu, Counter::FlushChecks), Get(cpu, Counter::FlushCheckNs) / 1.0e6);
        }
    }
    LOG_INFO(Render, "D3D12 guest GPU wait sites: {}", DescribeGuestWaitSites());
    LOG_INFO(Render, "D3D12 macro profiles: {}", DescribeMacroProfiles());
    // Who submits and waits (S/W, count, eden-uwp.exe RVAs from the innermost caller out); only
    // recorded with the detailed GPU profile.
    if (VideoCore::Perf::DetailedGpuProfileEnabled()) {
        LOG_INFO(Render, "D3D12 sync sites: {}", scheduler.TakeSyncSites(6));
    }
    const DXGI_QUERY_VIDEO_MEMORY_INFO video = device.QueryVideoMemory();
    LOG_INFO(Render,
             "D3D12 memory: GPU {} MiB, DXGI budget {} MiB, cache budget {} MiB, caches see {} MiB used",
             video.CurrentUsage >> 20, video.Budget >> 20, device.CacheMemoryBudget() >> 20,
             device.CacheMemoryUsage() >> 20);
}

} // namespace D3D12
