// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string_view>

#include "common/common_types.h"

/// Statistical CPU profiler that runs inside the process, for where no external one can attach
/// (the Xbox in Developer Mode: no xperf, no PIX for UWP).
///
/// A thread of its own suspends every named thread (Common::SetCurrentThreadName registers them)
/// at a fixed rate, reads its registers and copies its stack, and resumes it. Nothing allocates or
/// locks while a thread is suspended, since it may hold the heap lock or any other: the stack is
/// unwound afterwards, on the copy (RtlVirtualUnwind, with the pointers into the stack moved to
/// the copy). JIT code is unwound through the function table Dynarmic registers.
///
/// Each window it appends to a text file: the CPU time of every thread, and every distinct stack
/// with how often it was seen, running or waiting. Addresses are module+RVA, so the PC resolves
/// them with the build's PDB (tools/xbox/profile-report.py).
namespace Common::SamplingProfiler {

/// Adds the calling thread to the threads that are sampled. Cheap; safe before Start.
void RegisterCurrentThread(std::string_view name);

struct Options {
    /// Samples per second of each running thread. Each sample pauses its thread for some 20-30 us,
    /// so 250 Hz costs a running thread under 1%; blocked threads are sampled 32 times less.
    u32 hz{250};
    /// Seconds per report window.
    u32 window_seconds{20};
    /// File the windows are appended to; truncated by Start.
    std::filesystem::path output;
};

/// Starts sampling. No-op when already running.
void Start(const Options& options);

/// Stops sampling and writes the window in progress.
void Stop();

[[nodiscard]] bool Running();

} // namespace Common::SamplingProfiler
