// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include "common/common_types.h"
#include "common/polyfill_thread.h"
#ifdef _WIN32
#include "common/windows/precise_timer.h"
#endif

namespace Common {

#ifdef _WIN32
/// Whether Event::WaitFor sleeps on a high-resolution timer where the system has one (the
/// default) or only on timer ticks, waiting the last tick on the CPU. Read by each Event's first
/// timed wait.
void SetPreciseTimerSleeps(bool enabled);
[[nodiscard]] bool IsPreciseTimerSleeps();
#endif

class Event {
public:
    void Set() {
        std::scoped_lock lk{mutex};
        if (!IsSet()) {
            is_set = 1;
            condvar.notify_one();
#ifdef _WIN32
            WakeTimedWaiter();
#endif
        }
    }

    void Wait() {
        std::unique_lock lk{mutex};
        condvar.wait(lk, [&] { return IsSet(); });
        is_set = 0;
    }

    /// On Windows, accurate to tens of microseconds: it sleeps on a high-resolution timer until
    /// shortly before the deadline (by how late that timer has been waking) and waits the rest on
    /// the CPU; without such timers, the last stretch it waits on the CPU is the OS timer
    /// resolution. One waiter.
    bool WaitFor(const std::chrono::nanoseconds time);

    template<class Clock, class Duration>
    bool WaitUntil(const std::chrono::time_point<Clock, Duration> time) {
        std::unique_lock lk{mutex};
        if (!condvar.wait_until(lk, time, [this] { return IsSet(); }))
            return false;
        is_set = 0;
        return true;
    }

    void Reset() {
        std::unique_lock lk{mutex};
        // no other action required, since wait loops on the predicate and any lingering signal will
        // get cleared on the first iteration
        is_set = 0;
    }

    [[nodiscard]] bool IsSet() const {
        return is_set.load() != 0;
    }

private:
#ifdef _WIN32
    /// WaitFor blocks on is_set itself (WaitOnAddress) or on its timer, not on the condition
    /// variable.
    void WakeTimedWaiter();
#endif

    /// A word, not a bool, so WaitFor can wait on it in the OS and on the CPU.
    alignas(64) std::atomic<u32> is_set{0};
#ifdef _WIN32
    /// WaitFor's timer and how late it wakes (host ticks), made by the first wait; the waiter is
    /// asleep on it while timer_sleeping.
    std::unique_ptr<Windows::PreciseTimer> timer;
    std::optional<Windows::TimerSlack> timer_slack;
    std::atomic<bool> timer_sleeping{false};
#endif
    std::condition_variable condvar;
    std::mutex mutex;
};

class Barrier {
public:
    explicit Barrier(std::size_t count_) : count(count_) {}

    /// Blocks until all "count" threads have called Sync()
    bool Sync(std::stop_token token = {}) {
        std::unique_lock lk{mutex};
        const std::size_t current_generation = generation;

        if (++waiting == count) {
            generation++;
            waiting = 0;
            condvar.notify_all();
            return true;
        } else {
            condvar.wait(lk, token,
                        [this, current_generation] { return current_generation != generation; });
            return !token.stop_requested();
        }
    }

private:
    std::condition_variable_any condvar;
    std::mutex mutex;
    std::size_t count;
    std::size_t waiting = 0;
    std::size_t generation = 0; // Incremented once each time the barrier is used
};

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

enum class ThreadPlacement : u32 {
    Default = 0,
    Background = 1,
    Efficiency = 2,
};

void SetCurrentThreadPriority(ThreadPriority new_priority);
void SetCurrentThreadName(const char* name);
void SetCurrentThreadToPerformanceCores();
void SetCurrentThreadToEfficiencyCores();
void SetCurrentThreadToBackgroundWork();
void SetCurrentThreadToAllCores();

void RefreshThreadPolicies();
void PollThreadPolicies();

} // namespace Common
