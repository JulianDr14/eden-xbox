// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include "common/thread_cpu_time.h"

int main() {
    using Clock = std::chrono::steady_clock;
    const auto start = Common::CurrentThreadCpuTimeNs();
    assert(start);
    const auto until = Clock::now() + std::chrono::milliseconds{100};
    while (Clock::now() < until) std::atomic_signal_fence(std::memory_order_seq_cst);
    const auto busy = Common::CurrentThreadCpuTimeNs();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto slept = Common::CurrentThreadCpuTimeNs();
    assert(busy && slept && *busy >= *start && *slept >= *busy);
    assert(*busy - *start > *slept - *busy);
    const auto bench = Clock::now();
    for (int n = 0; n < 100000; ++n) assert(Common::CurrentThreadCpuTimeNs());
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - bench).count();
    std::cout << "Thread CPU gate passed; busy " << (*busy - *start) / 1000000.0
              << " ms, sleep " << (*slept - *busy) / 1000000.0 << " ms; query " << ns / 100000.0 << " ns\n";
}
