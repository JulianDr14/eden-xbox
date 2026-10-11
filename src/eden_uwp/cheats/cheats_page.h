// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The cheats page, in the library (before playing) and in the in-game menu (while playing): a tab
// per category with cheats, a list of switches, and what a press did. The model is a CheatSession
// in the app and a plain catalog in the tests; both pages draw from this state.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "eden_uwp/cheats/cheat_catalog.h"

namespace EdenXbox::Cheats {

/// Geometry of the page's panel, from its top-left corner (layout units of the 1280x720 screen).
struct CheatsLayout {
    static constexpr float Width = 1060, Height = 650, Margin = 40;
    static constexpr float TabsTop = 112, TabHeight = 38;
    static constexpr float RowTop = 166, RowStride = 52, RowHeight = 46;
    static constexpr std::size_t VisibleRows = 7;
    static constexpr float Status = 538, Note = 578, Footer = 606;
};

struct CheatsPage {
    CheatCategory category{CheatCategory::Gameplay};
    std::size_t selected{}; ///< Row in the category's list.
    std::size_t first{};    ///< First row shown.
};

enum class CheatsInput : std::uint8_t {
    Up, Down, PreviousCategory, NextCategory, Toggle, ToggleAll, Refresh, Back,
};

enum class CheatsEffect : std::uint8_t {
    None,
    Moved,      ///< Only the cursor or the tab changed.
    Changed,    ///< Cheats changed (and were saved): apply them to a running game.
    OverBudget, ///< Nothing changed: the VM has no room for it.
    Refresh,    ///< Ask the online database again.
    Back,
};

/// The categories that have cheats, in tab order.
inline std::vector<CheatCategory> CheatTabs(const CheatCatalog& catalog) {
    std::vector<CheatCategory> tabs;
    for (const auto category : {CheatCategory::Gameplay, CheatCategory::Graphics, CheatCategory::Mine})
        if (!catalog.Cheats(category).empty()) tabs.push_back(category);
    return tabs;
}

/// Keeps the page on a category with cheats and the cursor inside its list, after the catalog
/// changed (cheats arrived from the database) or the page opened.
inline void ClampCheatsPage(CheatsPage& page, const CheatCatalog& catalog, std::size_t visible_rows) {
    const auto tabs = CheatTabs(catalog);
    if (!tabs.empty() && std::find(tabs.begin(), tabs.end(), page.category) == tabs.end()) {
        page.category = tabs.front();
        page.selected = page.first = 0;
    }
    const std::size_t count = catalog.Cheats(page.category).size();
    page.selected = count ? std::min(page.selected, count - 1) : 0;
    if (page.selected < page.first) page.first = page.selected;
    if (visible_rows && page.selected >= page.first + visible_rows) page.first = page.selected + 1 - visible_rows;
    page.first = std::min(page.first, count > visible_rows ? count - visible_rows : 0);
}

/// Applies one press. `Model` gives Catalog(), Toggle(CheatRef) and SetCategory(category, on).
template <typename Model>
CheatsEffect HandleCheatsInput(CheatsPage& page, Model& model, CheatsInput input,
                               std::size_t visible_rows) {
    const CheatCatalog& catalog = model.Catalog();
    const auto cheats = catalog.Cheats(page.category);
    const auto result = [](ToggleResult toggled) {
        return toggled == ToggleResult::Changed    ? CheatsEffect::Changed
               : toggled == ToggleResult::OverBudget ? CheatsEffect::OverBudget
                                                     : CheatsEffect::None;
    };
    switch (input) {
    case CheatsInput::Up:
    case CheatsInput::Down:
        if (cheats.empty()) return CheatsEffect::None;
        page.selected = input == CheatsInput::Up ? (page.selected + cheats.size() - 1) % cheats.size()
                                                 : (page.selected + 1) % cheats.size();
        ClampCheatsPage(page, catalog, visible_rows);
        return CheatsEffect::Moved;
    case CheatsInput::PreviousCategory:
    case CheatsInput::NextCategory: {
        const auto tabs = CheatTabs(catalog);
        if (tabs.size() < 2) return CheatsEffect::None;
        const auto at = static_cast<std::size_t>(std::find(tabs.begin(), tabs.end(), page.category) - tabs.begin());
        const std::size_t next = input == CheatsInput::PreviousCategory ? (at + tabs.size() - 1) % tabs.size()
                                                                       : (at + 1) % tabs.size();
        page = CheatsPage{tabs[next]};
        return CheatsEffect::Moved;
    }
    case CheatsInput::Toggle:
        if (cheats.empty()) return CheatsEffect::None;
        return result(model.Toggle(cheats[page.selected]));
    case CheatsInput::ToggleAll:
        // All on, unless they all are: then all off.
        if (cheats.empty()) return CheatsEffect::None;
        return result(model.SetCategory(page.category,
                                        catalog.EnabledCount(page.category) < cheats.size()));
    case CheatsInput::Refresh:
        return CheatsEffect::Refresh;
    case CheatsInput::Back:
        return CheatsEffect::Back;
    }
    return CheatsEffect::None;
}

} // namespace EdenXbox::Cheats
