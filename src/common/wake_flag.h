// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/cpu_wait.h"

namespace Common {

/// Whether and how long a waiter spins on the CPU before blocking in the OS.
struct SpinPolicy {
    enum class Mode : u8 {
        Off,      ///< block at once, as a condition variable does
        Fixed,    ///< always spin up to the limit
        Adaptive, ///< spin up to the limit while recent waits ended within it
    };
    Mode mode{Mode::Adaptive};
    std::chrono::microseconds limit{200};

    /// "off", "adaptive", "adaptive:<us>" or "fixed:<us>".
    [[nodiscard]] static std::optional<SpinPolicy> Parse(std::string_view text);
    [[nodiscard]] std::string Describe() const;
};

/// Picks each wait's spin budget from how long recent waits lasted, so spinning stops costing CPU
/// when waits are long (menus, loading) and resumes when they shorten. One waiter; not
/// thread-safe.
///
/// The hit rate counts waits that ended within the limit, whatever the budget was, so it does not
/// depend on the budget it chooses. Below one in four it spins only an eighth of the limit.
class AdaptiveSpinBudget {
public:
    explicit AdaptiveSpinBudget(SpinPolicy policy) noexcept;

    /// Host ticks (WallClock::GetUptime) to spin before the next block; 0 blocks at once.
    [[nodiscard]] s64 Next() const noexcept;

    /// How long, in host ticks, the wait Next() budgeted lasted until the flag was raised.
    void Record(s64 waited) noexcept;

private:
    static constexpr u32 RATE_ONE = 256;            ///< hit rate fixed point
    static constexpr u32 RATE_SPIN = RATE_ONE / 4;  ///< below it, waits mostly outlast the spin
    static constexpr u32 RATE_SHIFT = 3;            ///< averages over about eight waits

    SpinPolicy::Mode mode;
    s64 full;
    s64 probe;
    u32 hit_rate{RATE_ONE};
};

/// A level-triggered flag one thread waits for and any thread raises, with a hybrid wait: the
/// waiter spins on the CPU for its AdaptiveSpinBudget and only then blocks in the OS
/// (std::atomic::wait), and raising only enters the OS to wake a blocked waiter. Handing work to
/// a waiter that is still spinning costs no system call on either side and no OS wake latency.
class alignas(64) WakeFlag {
public:
    enum class WaitResult : u8 {
        AlreadyRaised, ///< raised before the wait began
        Spun,          ///< raised while spinning
        Blocked,       ///< raised after blocking in the OS
    };

    explicit WakeFlag(SpinPolicy policy = {},
                      CpuWaitMethod method = BestCpuWaitMethod()) noexcept;

    void Raise() noexcept;
    /// For the waiting thread, after Wait returns: as with a condition variable, a clear from
    /// another thread can hide a raise from a blocked waiter.
    void Clear() noexcept;
    [[nodiscard]] bool IsRaised() const noexcept;

    /// Returns once the flag is raised, leaving it raised. One waiting thread at a time.
    WaitResult Wait() noexcept;

private:
    static constexpr u32 RAISED = 1;
    static constexpr u32 BLOCKED = 2; ///< the waiter is (about to be) blocked in the OS

    /// Blocks in the OS until the flag is raised.
    void Block() noexcept;

    /// Alone on its cache line (the class is aligned to one): monitor waits wake on any store to
    /// the line.
    std::atomic<u32> state{};
    CpuWaitMethod method;
    AdaptiveSpinBudget budget;
};

} // namespace Common
