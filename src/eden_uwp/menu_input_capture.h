// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace EdenXbox {
// Applied to logical input after mapping/keyboard/script and before controller style.
// A press consumed by a modal stays consumed until release, even after it closes.
class MenuInputCapture {
public:
    template <typename State>
    void Filter(State& state, bool open) {
        const bool left = state.left_x != 0.0f || state.left_y != 0.0f;
        const bool right = state.right_x != 0.0f || state.right_y != 0.0f;
        if (open) {
            buttons = state.buttons;
            left_stick = left;
            right_stick = right;
            state = {};
            return;
        }
        buttons &= state.buttons;
        state.buttons &= ~buttons;
        left_stick &= left;
        right_stick &= right;
        if (left_stick) state.left_x = state.left_y = 0.0f;
        if (right_stick) state.right_x = state.right_y = 0.0f;
    }
private:
    std::uint32_t buttons{};
    bool left_stick{}, right_stick{};
};
}
