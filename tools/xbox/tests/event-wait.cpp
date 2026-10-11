// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

// Event::WaitFor gate (the host timing thread's wait): timeouts end on time and never early,
// waits of any length keep the core mostly free when high-resolution timers exist (the timer
// slack it learns follows late wakes up and back down), and Set wakes the waiter in every stage
// of the wait (asleep on the timer or blocked, waiting on the CPU) without ever being lost.
// Run with build-uwp/diagnostics/event-wait/run.bat.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include "common/cpu_features.h"
#include "common/cpu_wait.h"
#include "common/thread.h"
#include "common/windows/precise_timer.h"
#include "common/windows/timer_resolution.h"

// common.lib asserts through these; the harness has no logging backend.
void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
void UnreachableAt(const char*) { std::abort(); }

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Micros = std::chrono::duration<double, std::micro>;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        std::exit(1);
    }
}

double Percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<size_t>(p * values.size()))];
}

u64 ThreadCycles() {
    ULONG64 cycles{};
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

void TestBasics() {
    Common::Event event;
    Check(!event.WaitFor(0ns), "a zero wait times out");
    Check(!event.WaitFor(-5ms), "a negative wait times out");
    event.Set();
    const auto start = Clock::now();
    Check(event.WaitFor(50ms), "a set event is consumed at once");
    Check(Clock::now() - start < 1ms, "a set event does not wait");
    Check(!event.IsSet(), "waiting consumes the event");
    Check(!event.WaitFor(1ms), "a consumed event times out again");
}

void TestTimerSlack() {
    // In nanoseconds, as Event::WaitFor uses it.
    Common::Windows::TimerSlack slack{1'250'000, 20'000, 1'250'000};
    Check(slack.Margin() == 1'250'000, "starts at the initial margin");
    for (int i = 0; i < 3000; ++i) {
        slack.Record(50'000);
    }
    Check(slack.Margin() >= 50'000 && slack.Margin() < 50'300, "settles on steady lateness");
    slack.Record(5'000'000);
    Check(slack.Margin() > 600'000 && slack.Margin() < 700'000,
          "a late wake raises it halfway to the cap");
    for (int i = 0; i < 3000; ++i) {
        slack.Record(0);
    }
    Check(slack.Margin() == 20'000, "never below the minimum");
    slack.Record(-30'000);
    Check(slack.Margin() == 20'000, "an early wake counts as on time");
}

void TestTimeouts() {
    Common::Event event;
    const bool native = Common::g_wall_clock.IsNative();
    for (const auto duration : {200us, 700us, 1500us, 5000us, 16667us}) {
        const int rounds = duration < 2ms ? 200 : 60;
        std::vector<double> late;
        u64 cycles = 0;
        Clock::duration wall{};
        for (int i = 0; i < rounds; ++i) {
            const u64 cycles_start = ThreadCycles();
            const auto start = Clock::now();
            Check(!event.WaitFor(duration), "an unset event times out");
            const auto elapsed = Clock::now() - start;
            cycles += ThreadCycles() - cycles_start;
            wall += elapsed;
            late.push_back(Micros(elapsed - duration).count());
        }
        // The TSC rate is estimated to 100 kHz, a few parts per million: never early by more.
        Check(*std::min_element(late.begin(), late.end()) > -20.0, "a timeout never ends early");
        const double cpu = native ? static_cast<double>(cycles) /
                                        (static_cast<double>(Common::g_wall_clock.rdtsc_frequency) *
                                         std::chrono::duration<double>(wall).count())
                                  : -1.0;
        std::cout << "  wait " << Micros(duration).count() << " us: late p50 "
                  << Percentile(late, 0.5) << " us, p99 " << Percentile(late, 0.99)
                  << " us, max " << Percentile(late, 1.0) << " us, CPU " << cpu * 100 << "%\n";
        // A desktop preempts now and then; the typical wait is what the gate holds.
        Check(Percentile(late, 0.9) < 50.0, "a timeout ends within 50 us (p90)");
        Check(Percentile(late, 0.99) < 1500.0, "a timeout ends within 1.5 ms (p99)");
        if (native && Common::Windows::PreciseTimer::IsSupported() && duration >= 700us) {
            // Only the timer's usual lateness waits on the CPU; the rest sleeps.
            Check(cpu < 0.35, "a wait sleeps on the timer and leaves the core mostly free");
        } else if (native && duration >= 5ms) {
            // The last stretch, about one timer tick, waits on the CPU; the rest sleeps.
            Check(cpu < 0.2, "a long wait leaves the core mostly free");
        }
    }
}

void TestSetWakes() {
    Common::Event event;
    std::atomic<s64> set_at{0};
    std::atomic<bool> done{false};
    std::vector<double> latency;
    constexpr int ROUNDS = 300;

    std::thread waiter{[&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        for (int i = 0; i < ROUNDS; ++i) {
            Check(event.WaitFor(40ms), "a set event wakes the waiter");
            const s64 woke = Clock::now().time_since_epoch().count();
            latency.push_back(Micros(Clock::duration{woke - set_at.load()}).count());
            done = true;
            done.notify_one();
        }
    }};

    std::mt19937 random{1234};
    // Spread over the whole wait: blocked in the condition variable, sleeping by ticks and
    // waiting on the CPU near the end of a 40 ms wait would need delays near 40 ms, so the
    // waiter's own timeout is long and the set lands anywhere from 0 to 12 ms in.
    std::uniform_int_distribution<int> delay_us{0, 12000};
    for (int i = 0; i < ROUNDS; ++i) {
        const auto until = Clock::now() + std::chrono::microseconds{delay_us(random)};
        while (Clock::now() < until) {
            std::this_thread::yield();
        }
        set_at = Clock::now().time_since_epoch().count();
        event.Set();
        done.wait(false);
        done = false;
    }
    waiter.join();
    std::cout << "  set to wake: p50 " << Percentile(latency, 0.5) << " us, p99 "
              << Percentile(latency, 0.99) << " us, max " << Percentile(latency, 1.0) << " us\n";
    Check(Percentile(latency, 0.5) < 200.0, "a set wakes the waiter within 200 us (p50)");
    Check(Percentile(latency, 0.99) < 2000.0, "a set wakes the waiter within 2 ms (p99)");
}

void TestSetNearDeadline() {
    // A set in the last stretch, while the waiter waits on the CPU or sleeps its last short
    // timer stretch before a 3 ms deadline, is seen at once too.
    Common::Event event;
    std::vector<double> latency;
    for (int i = 0; i < 100; ++i) {
        std::atomic<s64> set_at{0};
        const auto start = Clock::now();
        std::thread setter{[&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            const auto when = start + std::chrono::microseconds{2600 + (i % 10) * 20};
            while (Clock::now() < when) {
            }
            set_at = Clock::now().time_since_epoch().count();
            event.Set();
        }};
        Check(event.WaitFor(3ms), "a set in the last stretch wakes the waiter");
        latency.push_back(
            Micros(Clock::duration{Clock::now().time_since_epoch().count() - set_at.load()})
                .count());
        setter.join();
    }
    std::cout << "  set in the last stretch to wake: p50 " << Percentile(latency, 0.5)
              << " us, p99 " << Percentile(latency, 0.99) << " us\n";
    // Without high-resolution timers, a set just before the CPU stretch waits out the current
    // timer tick.
    // Asleep on a high-resolution timer, the wake goes through the OS.
    const double last_stretch_p50 = Common::Windows::PreciseTimer::IsSupported() ? 100.0 : 20.0;
    Check(Percentile(latency, 0.5) < last_stretch_p50,
          "a set in the last stretch wakes at once (p50)");
    Check(Percentile(latency, 0.99) < 1000.0, "a set in the last stretch wakes within 1 ms (p99)");
}

} // Anonymous namespace

int main() {
    // As the host timing thread (ThreadPriority::VeryHigh), so a busy desktop does not preempt it.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const auto resolution = Common::Windows::SetCurrentTimerResolutionToMaximum();
    std::cout << "clock " << (Common::g_wall_clock.IsNative() ? "native TSC" : "steady_clock")
              << " at " << Common::g_wall_clock.rdtsc_frequency / 1e6 << " MHz, CPU waits "
              << Common::CpuWaitMethodName(Common::BestCpuWaitMethod()) << ", timer resolution "
              << Micros(resolution).count() << " us, high-resolution timers "
              << (Common::Windows::PreciseTimer::IsSupported() ? "yes" : "no") << '\n';
    Check(!Common::g_cpu_caps.invariant_tsc || Common::g_wall_clock.IsNative(),
          "an invariant TSC gives a native clock");
    TestTimerSlack();
    TestBasics();
    TestTimeouts();
    TestSetWakes();
    TestSetNearDeadline();
    std::cout << "event wait: timeouts, CPU share, set wakes PASS\n";
    return 0;
}
