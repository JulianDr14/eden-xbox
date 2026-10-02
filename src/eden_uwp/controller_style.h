// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace EdenXbox {
// What the guest sees as player 1, independent of the physical pad (Eden's NpadStyleIndex). Some
// games refuse a Pro Controller (Let's Go accepts only handheld or a single Joy-Con), so the pad
// can be presented as another Switch controller, as Eden's "Controller type" setting does.
enum class ConsoleControllerStyle : std::uint8_t {
    Auto, Pro, DualJoycon, Handheld, LeftJoycon, RightJoycon,
};
inline constexpr std::size_t ConsoleControllerStyleCount = 6;

constexpr std::wstring_view ConsoleControllerStyleLabel(ConsoleControllerStyle style) {
    switch (style) {
    case ConsoleControllerStyle::Pro:         return L"Pro Controller";
    case ConsoleControllerStyle::DualJoycon:  return L"Joy-Con dobles";
    case ConsoleControllerStyle::Handheld:    return L"Modo portatil";
    case ConsoleControllerStyle::LeftJoycon:  return L"Joy-Con izquierdo";
    case ConsoleControllerStyle::RightJoycon: return L"Joy-Con derecho";
    default:                                  return L"Automatico (segun el juego)";
    }
}

constexpr ConsoleControllerStyle StepConsoleControllerStyle(ConsoleControllerStyle style, bool back) {
    const auto n = static_cast<std::size_t>(style);
    return static_cast<ConsoleControllerStyle>(
        (n + (back ? ConsoleControllerStyleCount - 1 : 1)) % ConsoleControllerStyleCount);
}

/// The part of hid's NpadStyleTag the automatic choice looks at.
struct SupportedControllerStyles {
    bool fullkey{}, joycon_dual{}, handheld{}, joycon_left{}, joycon_right{};
};

constexpr bool IsStyleSupported(ConsoleControllerStyle style, SupportedControllerStyles s) {
    switch (style) {
    case ConsoleControllerStyle::Pro:         return s.fullkey;
    case ConsoleControllerStyle::DualJoycon:  return s.joycon_dual;
    case ConsoleControllerStyle::Handheld:    return s.handheld;
    case ConsoleControllerStyle::LeftJoycon:  return s.joycon_left;
    case ConsoleControllerStyle::RightJoycon: return s.joycon_right;
    default:                                  return false;
    }
}

/// Automatic: the controller the game accepts that best fits a full pad. Eden itself only falls
/// back from Pro to dual Joy-Cons; handheld and a single Joy-Con cover the games refusing both.
constexpr ConsoleControllerStyle AutoControllerStyle(SupportedControllerStyles s) {
    if (s.fullkey) return ConsoleControllerStyle::Pro;
    if (s.joycon_dual) return ConsoleControllerStyle::DualJoycon;
    if (s.handheld) return ConsoleControllerStyle::Handheld;
    if (s.joycon_right) return ConsoleControllerStyle::RightJoycon;
    if (s.joycon_left) return ConsoleControllerStyle::LeftJoycon;
    return ConsoleControllerStyle::Pro;
}

/// VirtualGamepad button ids (virtual_gamepad.h); 20/21 are the right Joy-Con SL/SR, which
/// EmulatedController binds but the VirtualButton enum does not name.
namespace PadBit {
inline constexpr std::uint32_t A = 0, B = 1, X = 2, Y = 3, LStick = 4, RStick = 5, L = 6, R = 7,
                               ZL = 8, ZR = 9, Plus = 10, Minus = 11, Left = 12, Up = 13,
                               Right = 14, Down = 15, SLLeft = 16, SRLeft = 17, Home = 18,
                               Capture = 19, SLRight = 20, SRRight = 21;
inline constexpr std::uint32_t Count = 22;
} // namespace PadBit

struct StylePad {
    std::uint32_t buttons{};
    float left_x{}, left_y{}, right_x{}, right_y{};
};

/// A single Joy-Con is held sideways, and the guest rotates its raw (upright) readings itself.
/// Rotate the pad back: buttons keep their position on the pad (right face button = the Joy-Con's
/// rightmost button when sideways), the left stick drives the Joy-Con's stick and the shoulders
/// become SL/SR. Every other style passes the pad through unchanged.
constexpr StylePad MapPadToStyle(const StylePad& in, ConsoleControllerStyle style) {
    const bool left = style == ConsoleControllerStyle::LeftJoycon;
    if (!left && style != ConsoleControllerStyle::RightJoycon) return in;
    const auto has = [&](std::uint32_t bit) { return (in.buttons >> bit & 1U) != 0; };
    const auto set = [](std::uint32_t& out, std::uint32_t bit, bool on) {
        if (on) out |= 1U << bit;
    };
    StylePad out{};
    // Positions: Switch A is right, B bottom, X top, Y left.
    // Left Joy-Con, rail up: Down/Left/Right/Up sit right/bottom/top/left.
    // Right Joy-Con, rail up: X/A/Y/B sit right/bottom/top/left.
    set(out.buttons, left ? PadBit::Down : PadBit::X, has(PadBit::A));
    set(out.buttons, left ? PadBit::Left : PadBit::A, has(PadBit::B));
    set(out.buttons, left ? PadBit::Right : PadBit::Y, has(PadBit::X));
    set(out.buttons, left ? PadBit::Up : PadBit::B, has(PadBit::Y));
    set(out.buttons, left ? PadBit::SLLeft : PadBit::SLRight, has(PadBit::L) || has(PadBit::ZL));
    set(out.buttons, left ? PadBit::SRLeft : PadBit::SRRight, has(PadBit::R) || has(PadBit::ZR));
    set(out.buttons, left ? PadBit::Minus : PadBit::Plus, has(PadBit::Plus) || has(PadBit::Minus));
    set(out.buttons, left ? PadBit::LStick : PadBit::RStick, has(PadBit::LStick) || has(PadBit::RStick));
    set(out.buttons, PadBit::Home, has(PadBit::Home));
    set(out.buttons, PadBit::Capture, has(PadBit::Capture));
    // Left Joy-Con turned a quarter counter-clockwise, right one clockwise.
    if (left) {
        out.left_x = in.left_y;
        out.left_y = -in.left_x;
    } else {
        out.right_x = -in.left_y;
        out.right_y = in.left_x;
    }
    return out;
}
} // namespace EdenXbox
