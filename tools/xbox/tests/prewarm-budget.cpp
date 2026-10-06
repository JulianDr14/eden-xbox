// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <iostream>
#include "eden_uwp/prewarm_budget.h"

namespace {
using EdenXbox::PrewarmBudget;
constexpr std::uint64_t M = PrewarmBudget::MiB;

void TestDefaultsAndClamp() {
    assert(PrewarmBudget{}.MiBPerCore() == 64);
    assert(PrewarmBudget{}.BytesPerCore() == 64 * M);
    assert(PrewarmBudget{-3}.Step() == 0);
    assert(PrewarmBudget{99}.MiBPerCore() == 16);
}

void TestLowersOncePerSession() {
    PrewarmBudget budget;
    assert(!budget.Observe(2048 * M));
    assert(!budget.Observe(257 * M));
    assert(budget.Observe(255 * M)); // Wonder at the limit: 64 -> 32 for the next session
    assert(budget.MiBPerCore() == 32);
    assert(!budget.Observe(0)); // once per session, however low it goes
    assert(budget.MiBPerCore() == 32);
    assert(!budget.Finish(3600)); // a session that came close never raises it
    assert(budget.MinHeadroom() == 0);

    PrewarmBudget lowest{2};
    assert(!lowest.Observe(0) && lowest.MiBPerCore() == 16);
}

void TestRaisesAfterAComfortableSession() {
    PrewarmBudget budget{1};
    assert(!budget.Observe(1500 * M));
    assert(!budget.Finish(PrewarmBudget::RAISE_SECONDS - 1)); // too short to judge
    PrewarmBudget long_enough{2};
    assert(!long_enough.Observe(1500 * M));
    assert(long_enough.Finish(PrewarmBudget::RAISE_SECONDS));
    assert(long_enough.MiBPerCore() == 32);

    PrewarmBudget tight{1};
    assert(!tight.Observe(1023 * M)); // between the thresholds: keep
    assert(!tight.Finish(3600) && tight.MiBPerCore() == 32);

    PrewarmBudget top{};
    assert(!top.Observe(4096 * M));
    assert(!top.Finish(3600) && top.Step() == 0);

    PrewarmBudget unobserved{1}; // no samples: the minimum stays at its maximum
    assert(unobserved.Finish(PrewarmBudget::RAISE_SECONDS) && unobserved.Step() == 0);
}
} // namespace

int main() {
    TestDefaultsAndClamp();
    TestLowersOncePerSession();
    TestRaisesAfterAComfortableSession();
    std::cout << "PASS: defaults, clamping, lowering once per session, raising after a "
                 "comfortable session\n";
}
