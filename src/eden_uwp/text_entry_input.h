// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include "eden_uwp/ui_actions.h"
#include "eden_uwp/text_entry.h"

namespace EdenXbox {
// Shared semantic mapping for library and in-game keyboards. Platform adapters
// decide which physical keys produce these actions (+/- stay reserved in-game).
inline std::optional<TextEntry::Command> TextEntryCommand(MenuAction action) {
    using Command = TextEntry::Command;
    switch (action) {
    case MenuAction::Up: return Command::Up;
    case MenuAction::Down: return Command::Down;
    case MenuAction::Left: return Command::Left;
    case MenuAction::Right: return Command::Right;
    case MenuAction::Confirm: return Command::Press;
    case MenuAction::Back: return Command::Cancel;
    case MenuAction::Alternate: return Command::Delete;
    case MenuAction::Refresh: return Command::Space;
    case MenuAction::PreviousTab: return Command::Shift;
    case MenuAction::NextTab: return Command::Done;
    case MenuAction::Toggle: return std::nullopt;
    }
    return std::nullopt;
}
}
