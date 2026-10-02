// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cmath>

namespace EdenXbox {
enum class StickDirection { None, Up, Down, Left, Right };
// UI navigation only: never changes the gameplay stick/deadzone settings.
class StickNavigation {
public:
    using Clock = std::chrono::steady_clock;
    void Reset(bool require_center = true) {
        direction = StickDirection::None;
        blocked = require_center;
    }
    StickDirection Update(double x, double y, Clock::time_point now) {
        if (!std::isfinite(x) || !std::isfinite(y)) { Reset(); return StickDirection::None; }
        const auto ax = std::abs(x), ay = std::abs(y);
        if (ax < Release && ay < Release) { Reset(false); return StickDirection::None; }
        if (blocked) return StickDirection::None;
        // Preserve the current axis near a diagonal to avoid alternating rows.
        const bool horizontal = direction == StickDirection::Left || direction == StickDirection::Right;
        const bool vertical = direction == StickDirection::Up || direction == StickDirection::Down;
        const bool use_x = horizontal && ax >= Release && ax >= ay * 0.8 ? true :
                           vertical && ay >= Release && ay >= ax * 0.8 ? false : ax >= ay;
        const double value = use_x ? x : y;
        const auto candidate = use_x ? (value < 0 ? StickDirection::Left : StickDirection::Right) :
                                      (value < 0 ? StickDirection::Down : StickDirection::Up);
        const double threshold = candidate == direction ? Release : Engage;
        if (std::abs(value) < threshold) { direction = StickDirection::None; return StickDirection::None; }
        if (candidate != direction) {
            direction = candidate;
            next = now + std::chrono::milliseconds{350};
            return candidate;
        }
        if (now < next) return StickDirection::None;
        next = now + std::chrono::milliseconds{120}; // one step, even after a stalled frame
        return candidate;
    }
private:
    static constexpr double Engage = 0.55, Release = 0.35;
    StickDirection direction{StickDirection::None};
    Clock::time_point next{};
    bool blocked{};
};
}
