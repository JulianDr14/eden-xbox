// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/controller_style.h"

int main() {
    using namespace EdenXbox;
    using S = ConsoleControllerStyle;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    const auto bit = [](std::uint32_t b) { return 1U << b; };

    // Automatic choice: Pro first, then Eden's dual fallback, then handheld/single Joy-Con.
    check(AutoControllerStyle({.fullkey = true, .handheld = true}) == S::Pro);
    check(AutoControllerStyle({.joycon_dual = true, .handheld = true}) == S::DualJoycon);
    // Let's Go: handheld and single Joy-Cons only.
    check(AutoControllerStyle({.handheld = true, .joycon_left = true, .joycon_right = true}) ==
          S::Handheld);
    check(AutoControllerStyle({.joycon_left = true, .joycon_right = true}) == S::RightJoycon);
    check(AutoControllerStyle({.joycon_left = true}) == S::LeftJoycon);
    check(!IsStyleSupported(S::Auto, {.fullkey = true}));

    // Cycling visits every style and wraps both ways.
    check(StepConsoleControllerStyle(S::Auto, true) == S::RightJoycon);
    check(StepConsoleControllerStyle(S::RightJoycon, false) == S::Auto);
    auto style = S::Auto;
    for (std::size_t i = 0; i < ConsoleControllerStyleCount; ++i) style = StepConsoleControllerStyle(style, false);
    check(style == S::Auto);

    // Full-pad styles pass through untouched.
    const StylePad full{bit(PadBit::A) | bit(PadBit::ZL), 0.5f, -0.25f, 0.1f, 0.2f};
    for (const auto s : {S::Pro, S::DualJoycon, S::Handheld}) {
        const auto out = MapPadToStyle(full, s);
        check(out.buttons == full.buttons && out.left_x == full.left_x &&
              out.right_y == full.right_y);
    }

    // Left Joy-Con sideways: right face button is Down, shoulders are SL/SR, stick rotated.
    auto left = MapPadToStyle({bit(PadBit::A) | bit(PadBit::L) | bit(PadBit::ZR) | bit(PadBit::Plus),
                               1.0f, 0.0f, 0.7f, 0.7f}, S::LeftJoycon);
    check(left.buttons == (bit(PadBit::Down) | bit(PadBit::SLLeft) | bit(PadBit::SRLeft) |
                           bit(PadBit::Minus)));
    check(left.left_x == 0.0f && left.left_y == -1.0f); // Pushing right = Joy-Con "down".
    check(left.right_x == 0.0f && left.right_y == 0.0f);
    left = MapPadToStyle({bit(PadBit::B) | bit(PadBit::X) | bit(PadBit::Y), 0.0f, 1.0f}, S::LeftJoycon);
    check(left.buttons == (bit(PadBit::Left) | bit(PadBit::Right) | bit(PadBit::Up)));
    check(left.left_x == 1.0f); // Pushing up = toward the rail.

    // Right Joy-Con sideways: right face button is X, bottom is A, stick on the right stick.
    auto right = MapPadToStyle({bit(PadBit::A) | bit(PadBit::B) | bit(PadBit::R) | bit(PadBit::LStick),
                                1.0f, 0.0f}, S::RightJoycon);
    check(right.buttons == (bit(PadBit::X) | bit(PadBit::A) | bit(PadBit::SRRight) |
                            bit(PadBit::RStick)));
    check(right.right_x == 0.0f && right.right_y == 1.0f);
    check(right.left_x == 0.0f && right.left_y == 0.0f);
    right = MapPadToStyle({bit(PadBit::X) | bit(PadBit::Y) | bit(PadBit::Home), 0.0f, 1.0f}, S::RightJoycon);
    check(right.buttons == (bit(PadBit::Y) | bit(PadBit::B) | bit(PadBit::Home)));
    check(right.right_x == -1.0f);
    std::puts("controller style: auto choice/cycle/passthrough/single Joy-Con rotation PASS");
}
