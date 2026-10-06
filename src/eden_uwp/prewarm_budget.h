// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace EdenXbox {
/// How much code the JIT prewarm may compile per core, learned per game from its last sessions.
/// The prewarm ends before the game grows, so it cannot judge its own cost by the headroom at the
/// time: a game whose profile has filled up (Mario Wonder: 115 MiB on three cores, 1 GiB of commit)
/// only runs out minutes later. A session that comes close to the limit lowers the next one's
/// budget a step; a clean session with plenty to spare raises it back. Pure, no clocks or I/O.
class PrewarmBudget {
public:
    static constexpr std::uint64_t MiB = 1024 * 1024;
    /// Per core, from the default down. The plan warms gameplay and shared blocks first, so the
    /// lower steps keep the blocks that matter most. 64 MiB is the most for now: 115 cost a full
    /// profile (Mario Wonder) 1 GiB of a 5 GiB limit, and 64 is what Wonder was measured with.
    static constexpr std::array<std::uint32_t, 3> STEPS_MIB{64, 32, 16};
    /// A session whose headroom fell below this lowers the next one's budget, at once.
    static constexpr std::uint64_t LOW_HEADROOM = 256 * MiB;
    /// A clean session whose headroom never fell below this raises it back a step...
    static constexpr std::uint64_t HIGH_HEADROOM = 1024 * MiB;
    /// ...if it lasted long enough to have seen the game's heavier parts.
    static constexpr std::uint32_t RAISE_SECONDS = 180;

    /// `step` as stored (0 is the default budget); out of range values clamp.
    constexpr explicit PrewarmBudget(std::int32_t step_ = 0) noexcept
        : step{step_ < 0 ? 0U
                         : static_cast<std::uint32_t>(step_) >= STEPS_MIB.size()
                               ? static_cast<std::uint32_t>(STEPS_MIB.size() - 1)
                               : static_cast<std::uint32_t>(step_)} {}

    [[nodiscard]] constexpr std::uint32_t Step() const noexcept { return step; }
    [[nodiscard]] constexpr std::uint32_t MiBPerCore() const noexcept { return STEPS_MIB[step]; }
    [[nodiscard]] constexpr std::uint64_t BytesPerCore() const noexcept {
        return std::uint64_t{STEPS_MIB[step]} * MiB;
    }

    /// The headroom during play. True when the next session's budget just went down a step
    /// (once per session): store Step() now, the session may end in a crash or a kill.
    constexpr bool Observe(std::uint64_t headroom) noexcept {
        if (headroom < min_headroom) {
            min_headroom = headroom;
        }
        if (lowered || headroom >= LOW_HEADROOM) {
            return false;
        }
        lowered = true;
        if (step + 1 >= STEPS_MIB.size()) {
            return false;
        }
        ++step;
        return true;
    }

    /// The session ended cleanly after `seconds` of play. True when the budget went up a step.
    constexpr bool Finish(std::uint32_t seconds) noexcept {
        if (lowered || step == 0 || seconds < RAISE_SECONDS || min_headroom < HIGH_HEADROOM) {
            return false;
        }
        --step;
        return true;
    }

    [[nodiscard]] constexpr std::uint64_t MinHeadroom() const noexcept { return min_headroom; }

private:
    std::uint32_t step;
    std::uint64_t min_headroom{std::numeric_limits<std::uint64_t>::max()};
    bool lowered{};
};
} // namespace EdenXbox
