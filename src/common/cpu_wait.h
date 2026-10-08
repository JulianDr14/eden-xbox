// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <string_view>

#include "common/common_types.h"

namespace Common {

/// How a thread waits on the CPU, without entering the OS, for another thread's store.
enum class CpuWaitMethod : u8 {
    Pause,    ///< re-read the word with a pause (yield on ARM) hint between reads
    MonitorX, ///< AMD MONITORX/MWAITX (Zen 2 and later): the core rests until the line is written
    UMonitor, ///< Intel UMONITOR/UMWAIT (WAITPKG), C0.1: the same on recent Intel cores
};

/// The best method this processor runs. The monitor methods need the invariant TSC that host
/// ticks (WallClock::GetUptime) come from, since their timeouts count TSC ticks, and are tried
/// once: a hypervisor that reports them without allowing them falls back to Pause.
[[nodiscard]] CpuWaitMethod BestCpuWaitMethod() noexcept;

[[nodiscard]] std::string_view CpuWaitMethodName(CpuWaitMethod method) noexcept;

/// Waits on the CPU until one of mask's bits is set in word, or until the host tick deadline
/// (WallClock::GetUptime) passes. True when a bit was set. Never enters the OS; meant for waits
/// of tens to hundreds of microseconds, shorter than an OS sleep and wake.
bool CpuWaitForBits(const std::atomic<u32>& word, u32 mask, s64 deadline,
                    CpuWaitMethod method) noexcept;

} // namespace Common
