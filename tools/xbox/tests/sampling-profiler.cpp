// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// In-process sampling profiler gate: a busy thread is seen running with its whole call chain,
// a blocked one waiting, a thread running on a fiber stack (as the emulated cores do) and one
// with a deep stack are unwound too, and sampling never deadlocks a thread that allocates.
// Build and run (from a Developer prompt, at the repository root):
//   cl /nologo /std:c++20 /EHsc /O2 /Zi /utf-8 /DNOMINMAX /DFMT_HEADER_ONLY /Isrc
//      /I.cache\cpm\fmt\12.1.0\include
//      tools\xbox\tests\sampling-profiler.cpp src\common\windows\sampling_profiler.cpp
//      /Fe:build-uwp\diagnostics\sampling-profiler\sampling-profiler.exe
//      /Fo:build-uwp\diagnostics\sampling-profiler\ /link /DEBUG
//   build-uwp\diagnostics\sampling-profiler\sampling-profiler.exe
//   python tools\xbox\profile-report.py profile.txt
//      --exe build-uwp\diagnostics\sampling-profiler\sampling-profiler.exe

#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/windows/sampling_profiler.h"

namespace {

std::atomic<bool> running{true};
volatile unsigned sink;

__declspec(noinline) unsigned BusyLeaf(unsigned seed) {
    unsigned value = seed;
    for (int i = 0; i < 2000; ++i) {
        value = value * 1664525u + 1013904223u;
    }
    return value;
}

__declspec(noinline) unsigned BusyMiddle(unsigned depth, unsigned seed) {
    if (depth == 0) {
        return BusyLeaf(seed);
    }
    volatile char pad[64];
    pad[0] = static_cast<char>(depth);
    return BusyMiddle(depth - 1, seed + pad[0]) + 1;
}

__declspec(noinline) void BusyThread() {
    Common::SamplingProfiler::RegisterCurrentThread("busy");
    while (running.load(std::memory_order_relaxed)) {
        sink = BusyMiddle(6, sink);
    }
}

__declspec(noinline) unsigned DeepRecursion(unsigned depth) {
    // About 300 KiB of stack in all: past the copy limit's comfort zone, below its cap.
    volatile char frame[1024];
    frame[0] = static_cast<char>(depth);
    if (depth == 0) {
        return BusyLeaf(frame[0]);
    }
    return DeepRecursion(depth - 1) + frame[0];
}

__declspec(noinline) void DeepThread() {
    Common::SamplingProfiler::RegisterCurrentThread("deep");
    while (running.load(std::memory_order_relaxed)) {
        sink = DeepRecursion(290);
    }
}

std::mutex wait_mutex;
std::condition_variable wait_cv;

__declspec(noinline) void SleeperThread() {
    Common::SamplingProfiler::RegisterCurrentThread("sleeper");
    std::unique_lock lock{wait_mutex};
    wait_cv.wait(lock, [] { return !running.load(); });
}

__declspec(noinline) void AllocatorThread() {
    // Suspended mid-allocation, the heap lock is held: the profiler must not need it then.
    Common::SamplingProfiler::RegisterCurrentThread("allocator");
    while (running.load(std::memory_order_relaxed)) {
        auto block = std::make_unique<std::vector<int>>(1 + sink % 4096);
        sink = static_cast<unsigned>(block->size());
    }
}

void* main_fiber;

__declspec(noinline) void WINAPI FiberWork(void*) {
    while (running.load(std::memory_order_relaxed)) {
        sink = BusyMiddle(3, sink);
    }
    SwitchToFiber(main_fiber);
}

__declspec(noinline) void FiberThread() {
    Common::SamplingProfiler::RegisterCurrentThread("fiber");
    main_fiber = ConvertThreadToFiber(nullptr);
    void* const fiber = CreateFiber(256 * 1024, FiberWork, nullptr);
    SwitchToFiber(fiber);
    DeleteFiber(fiber);
    ConvertFiberToThread();
}

} // Anonymous namespace

int main() {
    const std::string path = "profile.txt";
    std::vector<std::thread> threads;
    threads.emplace_back(BusyThread);
    threads.emplace_back(DeepThread);
    threads.emplace_back(SleeperThread);
    threads.emplace_back(AllocatorThread);
    threads.emplace_back(FiberThread);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Common::SamplingProfiler::Start({.hz = 1000, .window_seconds = 2, .output = path});
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));
    Common::SamplingProfiler::Stop();

    running = false;
    wait_cv.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }

    std::ifstream file{path};
    std::string line;
    unsigned windows = 0;
    unsigned long long busy_running = 0, sleeper_waiting = 0, fiber_running = 0;
    unsigned long long deep_running = 0, allocator_running = 0, sleeper_running = 0;
    size_t busy_max_depth = 0, deep_max_depth = 0, fiber_max_depth = 0;
    while (std::getline(file, line)) {
        if (line.starts_with("window ")) {
            ++windows;
            continue;
        }
        if (!line.starts_with("stack ")) {
            continue;
        }
        std::istringstream tokens{line};
        std::string tag, thread, state, frames;
        unsigned long long count{};
        tokens >> tag >> thread >> state >> count >> frames;
        const size_t depth = static_cast<size_t>(std::count(frames.begin(), frames.end(), ';')) + 1;
        if (thread == "busy" && state == "R") {
            busy_running += count;
            busy_max_depth = std::max(busy_max_depth, depth);
        } else if (thread == "sleeper" && state == "W") {
            sleeper_waiting += count;
        } else if (thread == "sleeper") {
            sleeper_running += count;
        } else if (thread == "fiber" && state == "R") {
            fiber_running += count;
            fiber_max_depth = std::max(fiber_max_depth, depth);
        } else if (thread == "deep" && state == "R") {
            deep_running += count;
            deep_max_depth = std::max(deep_max_depth, depth);
        } else if (thread == "allocator" && state == "R") {
            allocator_running += count;
        }
    }
    std::cout << "windows " << windows << ", busy " << busy_running << " samples (depth up to "
              << busy_max_depth << "), deep " << deep_running << " (depth up to " << deep_max_depth
              << "), fiber " << fiber_running << " (depth up to " << fiber_max_depth
              << "), allocator " << allocator_running << ", sleeper waiting " << sleeper_waiting
              << "\n";
    assert(windows >= 2);
    // About 1000 samples a second for 4.5 s; leave room for a loaded machine.
    assert(busy_running > 2000);
    // BusyLeaf + 7 BusyMiddle + BusyThread + the thread's start frames.
    assert(busy_max_depth >= 10);
    assert(deep_running > 1000 && deep_max_depth >= 90);
    assert(fiber_running > 1000 && fiber_max_depth >= 5);
    assert(allocator_running > 1000);
    // Blocked: its samples are waiting ones, though being suspended makes it run a little.
    assert(sleeper_waiting > 0 && sleeper_running * 20 < sleeper_waiting);
    std::cout << "PASS\n";
    return 0;
}
