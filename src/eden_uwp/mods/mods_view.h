// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mods page as drawn: header with what is on, what conflicts and what has updates, the three
// tabs, the tab's list or grid, a line under it that explains the selection in plain words (which
// mod wins which files, whether it fits the installed version, what downloads), and the sheets
// over it. The host draws the button prompts under it, at ModsLayout::Footer, from ModsPrompts.

#include <string_view>
#include <vector>

#include "eden_uwp/localization.h"
#include "eden_uwp/mods/mod_session.h"
#include "eden_uwp/mods/mods_page.h"

namespace EdenXbox {

class UiPainter;

namespace Mods {

struct ModsView {
    const ModSession& session;
    const ModsPage& page;
    std::wstring_view game;   ///< The game's name, under the title.
    std::wstring_view notice; ///< What the last action did, if it needs saying.
};

/// Draws the page in the ModsLayout panel, offset by (x, y).
void DrawModsPanel(UiPainter& paint, const ModsView& view, float x = 0, float y = 0);

I18n::Text ErrorText(ModError error);

/// The buttons that do something on the page as it is now.
struct ModsPrompt {
    enum class Button : std::uint8_t { A, B, X, Y, Bumpers, Menu };
    Button button;
    I18n::Text label;
};
std::vector<ModsPrompt> ModsPrompts(const ModSession& session, const ModsPage& page);

/// What the page needs of the session for one press (ModsCounts).
ModsCounts CountsFor(const ModSession& session, const ModsPage& page, std::wstring_view game_name);

} // namespace Mods
} // namespace EdenXbox
