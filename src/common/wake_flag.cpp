// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <charconv>

#include <fmt/format.h>

#include "common/cpu_features.h"
#include "common/wake_flag.h"

namespace Common {

std::optional<SpinPolicy> SpinPolicy::Parse(std::string_view text) {
    if (text == "off") {
        return SpinPolicy{Mode::Off, std::chrono::microseconds{0}};
    }
    if (text == "adaptive") {
        return SpinPolicy{};
    }
    const auto with_limit = [&](std::string_view prefix, Mode mode) -> std::optional<SpinPolicy> {
        if (!text.starts_with(prefix)) {
            return std::nullopt;
        }
        const std::string_view value = text.substr(prefix.size());
        u32 us{};
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), us);
        // A wait longer than a few milliseconds belongs in the OS; zero is "off".
        if (ec != std::errc{} || end != value.data() + value.size() || us == 0 || us > 5000) {
            return std::nullopt;
        }
        return SpinPolicy{mode, std::chrono::microseconds{us}};
    };
    if (auto policy = with_limit("adaptive:", Mode::Adaptive)) {
        return policy;
    }
    return with_limit("fixed:", Mode::Fixed);
}

std::string SpinPolicy::Describe() const {
    switch (mode) {
    case Mode::Off:
        return "no spin";
    case Mode::Fixed:
        return fmt::format("spin {} us", limit.count());
    case Mode::Adaptive:
        return fmt::format("adaptive spin up to {} us", limit.count());
    }
    return "unknown";
}

AdaptiveSpinBudget::AdaptiveSpinBudget(SpinPolicy policy) noexcept
    : mode{policy.mode},
      full{policy.mode == SpinPolicy::Mode::Off
               ? 0
               : static_cast<s64>(g_wall_clock.NsToTicks(policy.limit))},
      probe{full / 8} {}

s64 AdaptiveSpinBudget::Next() const noexcept {
    switch (mode) {
    case SpinPolicy::Mode::Off:
        return 0;
    case SpinPolicy::Mode::Fixed:
        return full;
    case SpinPolicy::Mode::Adaptive:
        return hit_rate >= RATE_SPIN ? full : probe;
    }
    return 0;
}

void AdaptiveSpinBudget::Record(s64 waited) noexcept {
    if (waited <= full) {
        hit_rate += (RATE_ONE - hit_rate) >> RATE_SHIFT;
    } else {
        hit_rate -= hit_rate >> RATE_SHIFT;
    }
}

WakeFlag::WakeFlag(SpinPolicy policy, CpuWaitMethod method_) noexcept
    : method{method_}, budget{policy} {}

void WakeFlag::Raise() noexcept {
    if (state.fetch_or(RAISED, std::memory_order_acq_rel) & BLOCKED) {
        state.notify_one();
    }
}

void WakeFlag::Clear() noexcept {
    state.fetch_and(~RAISED, std::memory_order_release);
}

bool WakeFlag::IsRaised() const noexcept {
    return (state.load(std::memory_order_acquire) & RAISED) != 0;
}

WakeFlag::WaitResult WakeFlag::Wait() noexcept {
    if (IsRaised()) {
        return WaitResult::AlreadyRaised;
    }
    const s64 spin = budget.Next();
    if (spin == 0) {
        Block();
        return WaitResult::Blocked;
    }
    const s64 start = g_wall_clock.GetUptime();
    if (CpuWaitForBits(state, RAISED, start + spin, method)) {
        budget.Record(g_wall_clock.GetUptime() - start);
        return WaitResult::Spun;
    }
    Block();
    // Includes the OS wake latency, so a wait the spin limit just covers can count as a miss.
    budget.Record(g_wall_clock.GetUptime() - start);
    return WaitResult::Blocked;
}

void WakeFlag::Block() noexcept {
    // Announce the block in the same word Raise sets, so either Raise sees BLOCKED and wakes us,
    // or we see RAISED and never sleep.
    u32 current = state.fetch_or(BLOCKED, std::memory_order_acq_rel) | BLOCKED;
    while ((current & RAISED) == 0) {
        state.wait(current, std::memory_order_acquire);
        current = state.load(std::memory_order_acquire);
    }
    state.fetch_and(~BLOCKED, std::memory_order_relaxed);
}

} // namespace Common
