// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace EdenXbox {
// Semantic UI input shared by menu and text entry. Physical mapping belongs to
// the platform input adapter; page models own what each action does.
enum class MenuAction : std::uint8_t {
    Toggle, Up, Down, Left, Right, Confirm, Back, Alternate, Refresh, PreviousTab, NextTab,
};
}
