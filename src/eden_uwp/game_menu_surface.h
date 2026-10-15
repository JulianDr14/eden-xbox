// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The in-game menu drawn with real text: Direct2D on a CPU bitmap (WIC), so the game's D3D12
// device is not shared and nothing is resident on the GPU. The renderer copies the image over the
// paused frame. A surface lives only while the menu is open: closed, the menu costs nothing.

#include <memory>
#include <string_view>
#include <utility>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_menu_image.h"

namespace EdenXbox {

class GameMenu;
struct TextEntryPanelView;
namespace Cheats {
struct CheatsView;
}

class GameMenuSurface {
public:
    GameMenuSurface();
    ~GameMenuSurface();
    GameMenuSurface(const GameMenuSurface&) = delete;
    GameMenuSurface& operator=(const GameMenuSurface&) = delete;

    /// Physical face labels and colors, from the same selected pad as input.
    void SetPromptMapping(u8 mapping);

    /// The menu's list, for a screen of `screen` pixels.
    D3D12::MenuImage DrawMenu(const GameMenu& menu, std::wstring_view game, std::pair<u32, u32> screen);
    /// The cheats page.
    D3D12::MenuImage DrawCheats(const Cheats::CheatsView& view, std::pair<u32, u32> screen);
    /// The keyboard a game opened (game_keyboard.h); B's prompt only when the game lets it cancel.
    D3D12::MenuImage DrawKeyboard(const TextEntryPanelView& view, bool can_cancel, std::pair<u32, u32> screen);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace EdenXbox
