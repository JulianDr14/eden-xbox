// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

// Hybrid wait gate: SpinPolicy parsing, AdaptiveSpinBudget behaviour, no lost wakeups in a
// cross-thread ping-pong for every policy and CPU wait method, and the handoff latency of each
// (what an idle emulated core pays when another core hands it a guest thread).
// Run with build-uwp/diagnostics/wake-flag/run.bat.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include "common/cpu_features.h"
#include "common/wake_flag.h"

// common.lib asserts through these; the harness has no logging backend.
void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
void UnreachableAt(const char*) { std::abort(); }

namespace {

using namespace std::chrono_literals;
using Common::SpinPolicy;
using Common::WakeFlag;
using Clock = std::chrono::steady_clock;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        std::exit(1);
    }
}

void TestParse() {
    const auto off = SpinPolicy::Parse("off");
    Check(off && off->mode == SpinPolicy::Mode::Off, "parse off");
    const auto adaptive = SpinPolicy::Parse("adaptive");
    Check(adaptive && adaptive->mode == SpinPolicy::Mode::Adaptive && adaptive->limit == 200us,
          "parse adaptive");
    const auto custom = SpinPolicy::Parse("adaptive:75");
    Check(custom && custom->limit == 75us, "parse adaptive:75");
    const auto fixed = SpinPolicy::Parse("fixed:300");
    Check(fixed && fixed->mode == SpinPolicy::Mode::Fixed && fixed->limit == 300us,
          "parse fixed:300");
    for (const char* bad : {"", "on", "fixed", "fixed:", "fixed:0", "fixed:-5", "fixed:9000",
                            "adaptive:12x", "adaptive: 5"}) {
        Check(!SpinPolicy::Parse(bad), "reject malformed policy");
    }
}

void TestBudget() {
    Common::AdaptiveSpinBudget off{*SpinPolicy::Parse("off")};
    Check(off.Next() == 0, "off never spins");

    Common::AdaptiveSpinBudget fixed{*SpinPolicy::Parse("fixed:100")};
    const s64 full = fixed.Next();
    Check(full > 0, "fixed spins");
    for (int i = 0; i < 64; ++i) {
        fixed.Record(full * 10);
    }
    Check(fixed.Next() == full, "fixed ignores long waits");

    Common::AdaptiveSpinBudget adaptive{*SpinPolicy::Parse("adaptive:100")};
    Check(adaptive.Next() == full, "adaptive starts spinning the limit");
    for (int i = 0; i < 64; ++i) {
        adaptive.Record(full * 10);
    }
    Check(adaptive.Next() == full / 8, "long waits: spin an eighth of the limit");
    // Waits longer than the probe but within the limit (the case a budget-relative hit rate
    // would never leave): they count as hits even though the probe missed them.
    for (int i = 0; i < 16; ++i) {
        adaptive.Record(full / 2);
    }
    Check(adaptive.Next() == full, "waits within the limit restore the full spin");
    adaptive.Record(full);
    Check(adaptive.Next() == full, "a wait of exactly the limit is a hit");
}

void TestSingleWaits() {
    WakeFlag flag{*SpinPolicy::Parse("off")};
    flag.Raise();
    Check(flag.Wait() == WakeFlag::WaitResult::AlreadyRaised, "raised before the wait");
    flag.Clear();
    Check(!flag.IsRaised(), "clear");

    std::thread raiser([&] {
        std::this_thread::sleep_for(20ms);
        flag.Raise();
    });
    Check(flag.Wait() == WakeFlag::WaitResult::Blocked, "off blocks and is woken");
    raiser.join();
    flag.Clear();

    WakeFlag spinning{*SpinPolicy::Parse("fixed:5000")};
    std::thread quick([&] {
        std::this_thread::sleep_for(1ms);
        spinning.Raise();
    });
    Check(spinning.Wait() == WakeFlag::WaitResult::Spun, "raise within the spin");
    quick.join();
}

struct PingPongResult {
    double ns_per_handoff;
    u64 spun;
    u64 blocked;
};

/// Two threads hand a token back and forth, each idling on its own flag as an emulated core does
/// (wait, clear, raise the other), `rounds` times. `work` busy-waits between handoffs.
PingPongResult PingPong(SpinPolicy policy, Common::CpuWaitMethod method, int rounds,
                        std::chrono::microseconds work) {
    WakeFlag a{policy, method}, b{policy, method};
    std::atomic<u64> spun{}, blocked{};
    const auto busy = [work] {
        const auto until = Clock::now() + work;
        while (Clock::now() < until) {
        }
    };
    const auto side = [&](WakeFlag& mine, WakeFlag& other, bool starts) {
        u64 local_spun{}, local_blocked{};
        for (int i = 0; i < rounds; ++i) {
            if (!(starts && i == 0)) {
                const auto result = mine.Wait();
                local_spun += result == WakeFlag::WaitResult::Spun;
                local_blocked += result == WakeFlag::WaitResult::Blocked;
                mine.Clear();
            }
            busy();
            other.Raise();
        }
        spun += local_spun;
        blocked += local_blocked;
    };
    const auto start = Clock::now();
    std::atomic<bool> done{};
    std::thread watchdog([&] {
        const auto limit = Clock::now() + 60s;
        while (!done.load() && Clock::now() < limit) {
            std::this_thread::sleep_for(10ms);
        }
        Check(done.load(), "ping-pong finished (no lost wakeup)");
    });
    std::thread second([&] { side(b, a, false); });
    side(a, b, true);
    a.Wait(); // the last raise from the second thread
    second.join();
    done = true;
    watchdog.join();
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    return {ns / (2.0 * rounds), spun.load(), blocked.load()};
}

} // namespace

int main() {
    TestParse();
    TestBudget();
    TestSingleWaits();

    const auto best = Common::BestCpuWaitMethod();
    std::cout << "CPU: " << Common::g_cpu_caps.cpu_string << "; best CPU wait "
              << Common::CpuWaitMethodName(best) << "\n";
    std::vector<Common::CpuWaitMethod> methods{Common::CpuWaitMethod::Pause};
    if (best != Common::CpuWaitMethod::Pause) {
        methods.push_back(best);
    }

    // Correctness under contention: no work between handoffs, every policy and method.
    for (const char* text : {"off", "fixed:200", "adaptive", "adaptive:20"}) {
        const auto policy = *SpinPolicy::Parse(text);
        for (const auto method : methods) {
            if (policy.mode == SpinPolicy::Mode::Off && method != methods.front()) {
                continue; // never spins: the method does not matter
            }
            const auto r = PingPong(policy, method, 100000, 0us);
            std::cout << "  stress " << text << " / " << Common::CpuWaitMethodName(method)
                      << ": " << r.ns_per_handoff << " ns per handoff (" << r.spun
                      << " spun, " << r.blocked << " blocked)\n";
        }
    }

    // The emulated-core pattern: ~50 us of work between handoffs.
    std::cout << "Handoffs with 50 us of work between them (latency = time per handoff - 50 us):\n";
    for (const char* text : {"off", "adaptive"}) {
        const auto policy = *SpinPolicy::Parse(text);
        for (const auto method : methods) {
            if (policy.mode == SpinPolicy::Mode::Off && method != methods.front()) {
                continue;
            }
            const auto r = PingPong(policy, method, 20000, 50us);
            std::cout << "  " << text << " / " << Common::CpuWaitMethodName(method) << ": "
                      << (r.ns_per_handoff - 50000.0) / 1000.0 << " us overhead per handoff ("
                      << r.spun << " spun, " << r.blocked << " blocked)\n";
        }
    }
    std::cout << "Wake flag gate passed\n";
    return 0;
}
