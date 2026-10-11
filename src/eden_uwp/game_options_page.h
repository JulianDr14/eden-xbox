// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The selected game's options, opened from the library with Menu: a list of what can be set for
// that one game. Each option is a page of its own; adding one (per-game mods, per-game settings)
// is an enum value, its texts and icon below, and the page it opens.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "eden_uwp/localization.h"
#include "eden_uwp/ui_icon.h"

namespace EdenXbox {

enum class GameOption : std::uint8_t { Cheats, Updates, Dlc };

/// The options shown, in order.
inline std::span<const GameOption> GameOptions() {
    static constexpr std::array options{GameOption::Cheats, GameOption::Updates, GameOption::Dlc};
    return options;
}

inline I18n::Text GameOptionTitle(GameOption option) {
    switch (option) {
    case GameOption::Updates: return I18n::Text::ContentUpdates;
    case GameOption::Dlc: return I18n::Text::ContentDlc;
    case GameOption::Cheats: return I18n::Text::CheatsTitle;
    }
    return I18n::Text::CheatsTitle;
}

inline I18n::Text GameOptionHint(GameOption option) {
    switch (option) {
    case GameOption::Updates:
    case GameOption::Dlc: return I18n::Text::ContentCategoryHint;
    case GameOption::Cheats: return I18n::Text::CheatsHint;
    }
    return I18n::Text::CheatsHint;
}

inline UiIcon GameOptionIcon(GameOption option) {
    switch (option) {
    case GameOption::Updates:
    case GameOption::Dlc: return UiIcon::Folder;
    case GameOption::Cheats: return UiIcon::Cheats;
    }
    return UiIcon::Settings;
}

struct GameOptionsPage {
    bool open{};
    std::size_t game{};     ///< The library entry the options are for.
    std::size_t selected{}; ///< Row in GameOptions().
    std::optional<GameOption> page; ///< The option page open over the list.
};

enum class GameOptionsInput : std::uint8_t { Up, Down, Confirm, Back };

/// Applies one press to the list (an open option page takes its own input).
inline void HandleGameOptionsInput(GameOptionsPage& options, GameOptionsInput input) {
    const std::size_t count = GameOptions().size();
    switch (input) {
    case GameOptionsInput::Up:
        options.selected = (options.selected + count - 1) % count;
        break;
    case GameOptionsInput::Down:
        options.selected = (options.selected + 1) % count;
        break;
    case GameOptionsInput::Confirm:
        options.page = GameOptions()[options.selected];
        break;
    case GameOptionsInput::Back:
        options.open = false;
        break;
    }
}

/// Geometry shared by the canvas and pointer hit tests (1280x720 layout units).
struct GameOptionsLayout {
    static constexpr float PanelX = 140, PanelY = 72, PanelWidth = 1000, PanelHeight = 612;
    static constexpr float RowX = 180, RowWidth = 920, RowTop = 214, RowStride = 86, RowHeight = 74;
    static constexpr float Footer = 630;
};

} // namespace EdenXbox
