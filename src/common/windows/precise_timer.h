// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>

#include "common/common_types.h"

namespace Common::Windows {

/// Learns how late a timer wake lands, so a precise wait sleeps until that long before its
/// deadline and waits only the rest on the CPU. It follows lateness up by halves and down slowly,
/// so it tracks the high end of recent wakes rather than their average without jumping to the
/// worst one. Unit-agnostic (the caller picks host ticks or nanoseconds); one waiter, not
/// thread-safe.
class TimerSlack {
public:
    /// Starts at `initial`; Margin stays within [minimum, maximum].
    constexpr TimerSlack(s64 initial, s64 minimum, s64 maximum) noexcept
        : estimate{initial}, min{minimum}, max{maximum} {}

    /// How long before a deadline to stop sleeping.
    [[nodiscard]] constexpr s64 Margin() const noexcept {
        return std::clamp(estimate, min, max);
    }

    /// How late a timer wake came after the time it was set for.
    constexpr void Record(s64 late) noexcept {
        late = std::clamp<s64>(late, 0, max);
        if (late > estimate) {
            estimate += (late - estimate + 1) >> RISE_SHIFT;
        } else {
            estimate -= (estimate - late) >> DECAY_SHIFT;
        }
    }

private:
    static constexpr int RISE_SHIFT = 1;  ///< halfway to a later wake each time
    static constexpr int DECAY_SHIFT = 8; ///< forgets a late wake over a few hundred wakes

    s64 estimate;
    s64 min;
    s64 max;
};

/// A high-resolution waitable timer (Windows 10 1803+): it fires on its own deadline instead of
/// the next system timer tick, so a sleep of a few hundred microseconds ends within tens of them,
/// and the thread leaves the core while it sleeps. Wake ends a sleep early from another thread.
/// One sleeping thread.
class PreciseTimer {
public:
    PreciseTimer();
    ~PreciseTimer();

    PreciseTimer(const PreciseTimer&) = delete;
    PreciseTimer& operator=(const PreciseTimer&) = delete;

    /// Whether this system has high-resolution timers; without them nothing is created.
    [[nodiscard]] static bool IsSupported();

    [[nodiscard]] bool IsValid() const noexcept {
        return timer != nullptr && wake_event != nullptr;
    }

    /// Blocks until `duration` passes or Wake is called; true when woken. A Wake made while
    /// nobody sleeps ends the next sleep at once.
    bool Sleep(std::chrono::nanoseconds duration);

    void Wake();

private:
    void* timer{};
    void* wake_event{};
};

} // namespace Common::Windows
