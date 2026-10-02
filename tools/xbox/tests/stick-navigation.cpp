// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <cstdio>
#include <limits>
#include "eden_uwp/stick_navigation.h"
int main() {
    using namespace EdenXbox;
    using namespace std::chrono;
    StickNavigation input;
    const auto at = [](int ms) { return StickNavigation::Clock::time_point{milliseconds{ms}}; };
    const auto check = [](bool value) { if (!value) std::abort(); };
    check(input.Update(.3, .3, at(0)) == StickDirection::None);
    check(input.Update(.8, 0, at(1)) == StickDirection::Right);
    check(input.Update(.8, 0, at(350)) == StickDirection::None);
    check(input.Update(.8, 0, at(351)) == StickDirection::Right);
    check(input.Update(.8, 0, at(470)) == StickDirection::None);
    check(input.Update(.8, 0, at(471)) == StickDirection::Right);
    check(input.Update(.7, .75, at(500)) == StickDirection::None);
    check(input.Update(.7, .75, at(591)) == StickDirection::Right);
    check(input.Update(.4, 0, at(711)) == StickDirection::Right);
    check(input.Update(.2, 0, at(712)) == StickDirection::None);
    check(input.Update(0, -1, at(713)) == StickDirection::Down);
    check(input.Update(0, 1, at(714)) == StickDirection::Up);
    input.Reset();
    check(input.Update(0, 1, at(1000)) == StickDirection::None);
    check(input.Update(0, 0, at(1001)) == StickDirection::None);
    check(input.Update(-1, 0, at(1002)) == StickDirection::Left);
    check(input.Update(-1, 0, at(9000)) == StickDirection::Left);
    check(input.Update(-1, 0, at(9000)) == StickDirection::None);
    check(input.Update(std::numeric_limits<double>::quiet_NaN(), 1, at(9100)) == StickDirection::None);
    check(input.Update(1, 0, at(9101)) == StickDirection::None);
    std::puts("stick navigation: drift/hysteresis/diagonal/repeat/reversal/modal/stall/invalid PASS");
}
