// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging.h"
#include "core/arm/jit_prewarm_stats.h"
#include "dynarmic/interface/jit_profile.h"
#include "video_core/frame_trace.h"

namespace VideoCore::FrameTrace {
namespace {

struct Entry {
    u64 ns;
    u32 thread;
    Event event;
    u64 a;
    u64 b;
};

// Stable 60-FPS gameplay reaches ~17k events/s; reserve headroom for four seconds.
// Fixed storage (~4 MiB), with no allocations while recording.
constexpr size_t CAPACITY = 131072;

std::atomic_bool active{false};
std::atomic<CaptureState> capture_state{CaptureState::Idle};
std::atomic<u32> capture_id{0};
std::atomic<u32> vsyncs_left{0};
std::atomic<u32> next_entry{0};
std::atomic<u32> writers{0};
std::chrono::steady_clock::time_point start_time;
std::array<Entry, CAPACITY> entries;
Dynarmic::JitProfile::Snapshot jit_begin{};
bool jit_profiled{};
Core::JitPrewarm::MissSnapshot misses_begin{};

u32 CurrentThread() {
#ifdef _WIN32
    return static_cast<u32>(GetCurrentThreadId());
#else
    return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000000);
#endif
}

const char* Name(Event event) {
    switch (event) {
    case Event::Vsync:
        return "vsync";
    case Event::VsyncComposed:
        return "vsync-composed";
    case Event::ComposeWaitEnd:
        return "vsync-waited-gpu-thread";
    case Event::Composite:
        return "composite";
    case Event::QueueBuffer:
        return "game-queue-buffer";
    case Event::AcquireBuffer:
        return "display-acquire-buffer";
    case Event::ReleaseBuffer:
        return "display-release-buffer";
    case Event::DequeueWait:
        return "game-dequeue-wait";
    case Event::DequeueWaitEnd:
        return "game-dequeue-wait-end";
    case Event::GpuFenceWait:
        return "game-fence-wait";
    case Event::GpuFenceSignal:
        return "game-fence-signal";
    case Event::GpuSubmit:
        return "gpu-submit";
    case Event::GpuIdleEnd:
        return "gpu-thread-got-work";
    case Event::GuestSvcBegin:
        return "guest-svc-begin";
    case Event::GuestSvcEnd:
        return "guest-svc-end";
    case Event::GuestThreadReady:
        return "guest-thread-ready";
    case Event::VsyncSignal:
        return "guest-vsync-signal";
    }
    return "?";
}

void Dump(u32 count) {
    LOG_INFO(Render, "Frame trace: {} events (ms from the start, host thread, event, a, b)",
             count);
    for (u32 i = 0; i < count; ++i) {
        const Entry& entry = entries[i];
        LOG_INFO(Render, "FT {:9.3f} {:>6} {} {} {}", static_cast<double>(entry.ns) / 1e6,
                 entry.thread, Name(entry.event), entry.a, entry.b);
    }
    LOG_INFO(Render, "Frame trace: end");
}

} // Anonymous namespace

void Start(u32 vsyncs) {
    if (active.load() || capture_state.load() == CaptureState::Saving || vsyncs == 0) {
        return;
    }
    next_entry.store(0);
    vsyncs_left.store(vsyncs);
    jit_profiled = Dynarmic::JitProfile::enabled.load(std::memory_order_relaxed);
    jit_begin = Dynarmic::JitProfile::Read();
    misses_begin = Core::JitPrewarm::ReadMisses();
    Core::JitPrewarm::capture_active.store(true, std::memory_order_relaxed);
    start_time = std::chrono::steady_clock::now();
    const u32 id = capture_id.fetch_add(1) + 1;
    LOG_INFO(Render, "Frame trace: recording {} vsyncs (capture {})", vsyncs, id);
    capture_state.store(CaptureState::Recording);
    active.store(true);
}

bool Active() {
    return active.load(std::memory_order_relaxed);
}

CaptureStatus GetCaptureStatus() {
    return {capture_state.load(std::memory_order_relaxed),
            capture_id.load(std::memory_order_relaxed),
            vsyncs_left.load(std::memory_order_relaxed)};
}

void Mark(Event event, u64 a, u64 b) {
    if (!active.load(std::memory_order_relaxed)) {
        return;
    }
    writers.fetch_add(1);
    const u32 index = next_entry.fetch_add(1);
    if (index < CAPACITY) {
        entries[index] = {
            .ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start_time)
                                       .count()),
            .thread = CurrentThread(),
            .event = event,
            .a = a,
            .b = b,
        };
    }
    writers.fetch_sub(1);
    const bool last_vsync = event == Event::Vsync && vsyncs_left.fetch_sub(1) == 1;
    // Guest state events can be recorded with the kernel scheduler lock held. Defer the
    // dump to VSync even when full: formatting thousands of lines under that lock would
    // stall the threads whose wakeup latency we are measuring. Entries stay bounded.
    const bool full_at_vsync = event == Event::Vsync && index + 1 >= CAPACITY;
    if (!full_at_vsync && !last_vsync) {
        return;
    }
    auto expected = CaptureState::Recording;
    if (!capture_state.compare_exchange_strong(expected, CaptureState::Saving)) {
        return;
    }
    // Publish Saving before disabling marks: T must not reset entries while Dump reads them.
    active.store(false);
    Core::JitPrewarm::capture_active.store(false, std::memory_order_relaxed);
    // Let marks already past the active check finish writing.
    while (writers.load() != 0) {
        std::this_thread::yield();
    }
    // Sample before Dump: writing thousands of trace lines is outside the measured capture.
    const auto misses_end = Core::JitPrewarm::ReadMisses();
    const auto jit_end = Dynarmic::JitProfile::Read();
    for (size_t core = 0; core < misses_end.size(); ++core) {
        for (size_t reason = 0; reason < Core::JitPrewarm::MissNames.size(); ++reason) {
            const auto count = misses_end[core][reason] - misses_begin[core][reason];
            if (count != 0) {
                LOG_INFO(Render, "Frame trace JIT misses capture {} core {} {}: {} blocks",
                         capture_id.load(), core, Core::JitPrewarm::MissNames[reason], count);
            }
        }
    }
    if (jit_profiled) {
        const u32 id = capture_id.load();
        LOG_INFO(Render, "Frame trace JIT capture {}: elapsed sums across cores; phases overlap; "
                         "calls crossing borders are included at completion", id);
        for (size_t i = 0; i < jit_end.size(); ++i) {
            LOG_INFO(Render, "Frame trace JIT capture {} {}: {} calls, {:.3f} ms", id,
                     Dynarmic::JitProfile::names[i], jit_end[i].calls - jit_begin[i].calls,
                     (jit_end[i].ns - jit_begin[i].ns) / 1.0e6);
        }
    }
    Dump(std::min<u32>(next_entry.load(), static_cast<u32>(CAPACITY)));
    capture_state.store(full_at_vsync && !last_vsync ? CaptureState::Truncated : CaptureState::Saved);
}

} // namespace VideoCore::FrameTrace
