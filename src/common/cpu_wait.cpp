// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>

#include "common/cpu_features.h"
#include "common/cpu_wait.h"

#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
#include <excpt.h>
#include <intrin.h>
#else
#include <immintrin.h>
#include <x86intrin.h>
#endif
#endif

#if defined(ARCHITECTURE_x86_64) && (defined(__clang__) || defined(__GNUC__))
#define CPU_WAIT_TARGET(features) __attribute__((target(features)))
#else
#define CPU_WAIT_TARGET(features)
#endif

namespace Common {
namespace {

[[nodiscard]] bool AnySet(const std::atomic<u32>& word, u32 mask) noexcept {
    return (word.load(std::memory_order_acquire) & mask) != 0;
}

void RelaxCpu() noexcept {
#if defined(ARCHITECTURE_x86_64)
    _mm_pause();
#elif defined(ARCHITECTURE_arm64) && defined(_MSC_VER)
    __yield();
#elif defined(ARCHITECTURE_arm64)
    asm volatile("yield");
#endif
}

bool PauseWait(const std::atomic<u32>& word, u32 mask, s64 deadline) noexcept {
    while (!AnySet(word, mask)) {
        if (g_wall_clock.GetUptime() >= deadline) {
            return AnySet(word, mask);
        }
        RelaxCpu();
    }
    return true;
}

#ifdef ARCHITECTURE_x86_64

[[nodiscard]] s64 ReadTsc() noexcept {
    return static_cast<s64>(__rdtsc());
}

CPU_WAIT_TARGET("mwaitx")
bool MonitorXWait(const std::atomic<u32>& word, u32 mask, s64 deadline) noexcept {
    for (;;) {
        // Armed before the last check, so a store between that check and MWAITX still wakes it.
        _mm_monitorx(const_cast<std::atomic<u32>*>(&word), 0, 0);
        if (AnySet(word, mask)) {
            return true;
        }
        const s64 left = deadline - ReadTsc();
        if (left <= 0) {
            return false;
        }
        // ECX bit 1 enables the timer, a delta in TSC ticks of at most 32 bits. Hint 0 rests in
        // C1: under 0.1 us slower to wake than C0, and the SMT sibling gets the core.
        _mm_mwaitx(1u << 1, 0, static_cast<u32>(
                                   std::min<s64>(left, std::numeric_limits<u32>::max())));
    }
}

CPU_WAIT_TARGET("waitpkg")
bool UMonitorWait(const std::atomic<u32>& word, u32 mask, s64 deadline) noexcept {
    for (;;) {
        _umonitor(const_cast<std::atomic<u32>*>(&word));
        if (AnySet(word, mask)) {
            return true;
        }
        if (ReadTsc() >= deadline) {
            return false;
        }
        // Control 1 asks for C0.1, the faster-waking state. The deadline is an absolute TSC; the
        // OS may cut the wait shorter (IA32_UMWAIT_CONTROL), and the loop simply waits again.
        _umwait(1, static_cast<u64>(deadline));
    }
}

/// Runs one short wait of the method, so a #UD under a hypervisor that reports the feature
/// without allowing it is caught here and not on an emulated core.
CPU_WAIT_TARGET("mwaitx,waitpkg")
bool ProbeMonitorWait(CpuWaitMethod method) noexcept {
    alignas(64) u32 line{};
#ifdef _MSC_VER
    __try {
#endif
        if (method == CpuWaitMethod::MonitorX) {
            _mm_monitorx(&line, 0, 0);
            _mm_mwaitx(1u << 1, 0, 64);
        } else {
            _umonitor(&line);
            _umwait(1, __rdtsc() + 64);
        }
        return true;
#ifdef _MSC_VER
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

#endif // ARCHITECTURE_x86_64

} // namespace

CpuWaitMethod BestCpuWaitMethod() noexcept {
    static const CpuWaitMethod best = [] {
#ifdef ARCHITECTURE_x86_64
        if (g_wall_clock.IsNative()) {
            if (g_cpu_caps.monitorx && ProbeMonitorWait(CpuWaitMethod::MonitorX)) {
                return CpuWaitMethod::MonitorX;
            }
            if (g_cpu_caps.waitpkg && ProbeMonitorWait(CpuWaitMethod::UMonitor)) {
                return CpuWaitMethod::UMonitor;
            }
        }
#endif
        return CpuWaitMethod::Pause;
    }();
    return best;
}

std::string_view CpuWaitMethodName(CpuWaitMethod method) noexcept {
    switch (method) {
    case CpuWaitMethod::Pause:
        return "pause";
    case CpuWaitMethod::MonitorX:
        return "MWAITX";
    case CpuWaitMethod::UMonitor:
        return "UMWAIT";
    }
    return "unknown";
}

bool CpuWaitForBits(const std::atomic<u32>& word, u32 mask, s64 deadline,
                    CpuWaitMethod method) noexcept {
    switch (method) {
#ifdef ARCHITECTURE_x86_64
    case CpuWaitMethod::MonitorX:
        return MonitorXWait(word, mask, deadline);
    case CpuWaitMethod::UMonitor:
        return UMonitorWait(word, mask, deadline);
#endif
    default:
        return PauseWait(word, mask, deadline);
    }
}

} // namespace Common
