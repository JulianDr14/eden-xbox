// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

// SPSCWakeQueue gate (the guest to GPU thread command queue): every item arrives once and in
// order through bursts, a full queue and idle gaps, for every spin policy; a stop request wakes
// a blocked consumer; and the handoff cost against SPSCQueue.
// Run with build-uwp/diagnostics/spsc-wake-queue/run.bat.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include "common/bounded_threadsafe_queue.h"
#include "common/spsc_wake_queue.h"
#include "common/windows/timer_resolution.h"

// common.lib asserts through these; the harness has no logging backend.
void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
void UnreachableAt(const char*) { std::abort(); }

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Common::SpinPolicy;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        std::exit(1);
    }
}

struct Item {
    Item() = default;
    explicit Item(u64 value_, bool gap_) : value{value_}, gap{gap_} {}
    u64 value{};
    bool gap{};
    std::vector<u32> payload; ///< moved, as the command lists are
};

void TestOrder(const char* policy_text) {
    // A small capacity so the producer often finds the queue full.
    Common::SPSCWakeQueue<Item, 16> queue{*SpinPolicy::Parse(policy_text)};
    constexpr u64 COUNT = 200'000;
    std::jthread consumer{[&](std::stop_token stop) {
        Item item;
        for (u64 expected = 0; expected < COUNT; ++expected) {
            Check(queue.PopWait(item, stop), "pop until the last item");
            Check(item.value == expected, "items arrive in order");
            if (item.gap) {
                // Let the producer run ahead and fill the queue.
                std::this_thread::sleep_for(50us);
            }
        }
    }};
    for (u64 i = 0; i < COUNT; ++i) {
        if (i % 5000 == 0) {
            // An idle gap: the consumer spins out its budget and blocks.
            std::this_thread::sleep_for(2ms);
        }
        queue.EmplaceWait(i, i % 997 == 0);
    }
    consumer.join();
    Item left;
    Check(!queue.TryPop(left), "nothing left over");
    std::cout << "  order with " << policy_text << ": " << COUNT << " items PASS\n";
}

void TestStop() {
    Common::SPSCWakeQueue<Item, 16> queue;
    std::jthread consumer{[&](std::stop_token stop) {
        Item item;
        Check(!queue.PopWait(item, stop), "a stop wakes an empty wait");
    }};
    std::this_thread::sleep_for(20ms);
    consumer.request_stop();
    consumer.join();

    // Stopped with an item queued: the item still comes out.
    std::stop_source source;
    queue.EmplaceWait(7u, false);
    source.request_stop();
    Item item;
    Check(queue.PopWait(item, source.get_token()) && item.value == 7, "queued before the stop");
    std::cout << "  stop PASS\n";
}

template <typename Queue, typename Pop>
double HandoffUs(Queue& queue, Pop pop) {
    // One item at a time, the consumer idle between them, as the GPU thread mostly is.
    constexpr int ROUNDS = 2000;
    std::vector<double> latency;
    std::atomic<s64> pushed_at{0};
    std::atomic<int> received{0};
    std::jthread consumer{[&](std::stop_token stop) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        Item item;
        for (int i = 0; i < ROUNDS; ++i) {
            pop(queue, item, stop);
            latency.push_back(std::chrono::duration<double, std::micro>(
                                  Clock::duration{Clock::now().time_since_epoch().count() -
                                                  pushed_at.load()})
                                  .count());
            received.store(i + 1);
        }
    }};
    for (int i = 0; i < ROUNDS; ++i) {
        const auto until = Clock::now() + std::chrono::microseconds{20 + (i % 8) * 40};
        while (Clock::now() < until) {
        }
        pushed_at = Clock::now().time_since_epoch().count();
        queue.EmplaceWait(static_cast<u64>(i), false);
        while (received.load() <= i) {
        }
    }
    consumer.join();
    std::sort(latency.begin(), latency.end());
    return latency[latency.size() / 2];
}

void TestHandoff() {
    Common::SPSCQueue<Item> old_queue;
    const double old_us = HandoffUs(old_queue, [](auto& q, Item& item, std::stop_token stop) {
        q.PopWait(item, stop);
    });
    Common::SPSCWakeQueue<Item> new_queue;
    const double new_us = HandoffUs(new_queue, [](auto& q, Item& item, std::stop_token stop) {
        q.PopWait(item, stop);
    });
    std::cout << "  handoff p50: SPSCQueue " << old_us << " us, SPSCWakeQueue " << new_us
              << " us\n";
    Check(new_us <= old_us, "the wake queue hands off no slower");
}

} // Anonymous namespace

int main() {
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    for (const char* policy : {"off", "adaptive", "fixed:300"}) {
        TestOrder(policy);
    }
    TestStop();
    TestHandoff();
    std::cout << "spsc wake queue: order, full queue, stop, handoff PASS\n";
    return 0;
}
