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

constexpr size_t CAPACITY = 16384;

std::atomic_bool active{false};
std::atomic<u32> vsyncs_left{0};
std::atomic<u32> next_entry{0};
std::atomic<u32> writers{0};
std::chrono::steady_clock::time_point start_time;
std::array<Entry, CAPACITY> entries;

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
    if (active.load() || vsyncs == 0) {
        return;
    }
    next_entry.store(0);
    vsyncs_left.store(vsyncs);
    start_time = std::chrono::steady_clock::now();
    LOG_INFO(Render, "Frame trace: recording {} vsyncs", vsyncs);
    active.store(true);
}

bool Active() {
    return active.load(std::memory_order_relaxed);
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
    const bool full = index + 1 == CAPACITY;
    const bool last_vsync = event == Event::Vsync && vsyncs_left.fetch_sub(1) == 1;
    if (!full && !last_vsync) {
        return;
    }
    bool expected = true;
    if (!active.compare_exchange_strong(expected, false)) {
        return;
    }
    // Let marks already past the active check finish writing.
    while (writers.load() != 0) {
        std::this_thread::yield();
    }
    Dump(std::min<u32>(next_entry.load(), static_cast<u32>(CAPACITY)));
}

} // namespace VideoCore::FrameTrace
