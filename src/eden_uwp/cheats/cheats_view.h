// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The cheats page as drawn, the same in the library and in the in-game menu: header, a tab per
// category, the list of switches, the cheat engine's room and where the cheats came from. The
// host draws the button prompts under it, at CheatsLayout::Footer.

#include <string_view>

#include "eden_uwp/cheats/cheat_session.h"
#include "eden_uwp/cheats/cheats_page.h"

namespace EdenXbox {

class UiPainter;

namespace Cheats {

struct CheatsView {
    const CheatSession& session;
    const CheatsPage& page;
    std::wstring_view game;   ///< The game's name, under the title.
    std::wstring_view notice; ///< What the last press did, if it needs saying.
    bool running{};           ///< In the in-game menu: changes apply on resume.
};

/// Draws the page in the CheatsLayout panel, offset by (x, y).
void DrawCheatsPanel(UiPainter& paint, const CheatsView& view, float x = 0, float y = 0);

} // namespace Cheats
} // namespace EdenXbox
