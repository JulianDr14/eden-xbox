// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCore {
class RendererBase;
}

// The frontend's in-game menu as the renderer shows it: an image the frontend draws (with real
// text) and the renderer copies over the paused game's last frame. Kept free of the renderer's
// headers, which need Mesa's, so the frontend can include it.

namespace D3D12 {

/// RGBA8 pixels, rows packed (`width * 4` bytes each).
struct MenuImage {
    u32 width{};
    u32 height{};
    std::vector<u8> pixels;
    /// Whether the whole screen is filled with `fill` (0xRRGGBB) under the image instead of
    /// showing the guest's frame: a screen of its own, not a menu over the game.
    bool fill_screen{};
    u32 fill{};
};

/// The screen the menu is shown on (the back buffer's size), {0, 0} unless the renderer is the
/// D3D12 one. With the guest paused.
[[nodiscard]] std::pair<u32, u32> GameMenuScreenSize(VideoCore::RendererBase& renderer);

/// Shows `image` centered over the guest's last frame and presents at once, if the renderer is
/// the D3D12 one. GPU thread only (Tegra::GPU::RunOnGpuThread), with the guest paused.
void ShowGameMenu(VideoCore::RendererBase& renderer, MenuImage image);

/// Removes the menu and presents the guest frame again. GPU thread only.
void HideGameMenu(VideoCore::RendererBase& renderer);

} // namespace D3D12
