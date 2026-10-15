// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/core/memory/cheat_text.cpp.
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/cheats/cheats_page.h"
#include "eden_uwp/game_options_page.h"

namespace {
using namespace EdenXbox;
using namespace EdenXbox::Cheats;

/// What CheatSession gives the page, over a plain catalog.
struct Model {
    CheatCatalog catalog;
    int saves = 0;
    const CheatCatalog& Catalog() const { return catalog; }
    ToggleResult Toggle(CheatRef ref) {
        const auto result = catalog.Toggle(ref);
        saves += result == ToggleResult::Changed;
        return result;
    }
    ToggleResult SetCategory(CheatCategory category, bool on) { return catalog.SetCategory(category, on); }
};

CheatSet Set(std::string mod, int count, int words = 1) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += "[Cheat " + std::to_string(i) + "]\n";
        for (int w = 0; w < words; ++w) text += "00000000 ";
        text += "\n";
    }
    auto cheats = Core::Memory::TextCheatParser{}.Parse(text);
    for (auto& cheat : cheats) cheat.enabled = false;
    return {mod, CategoryOfMod(mod), std::move(cheats), {}};
}
} // namespace

int main() {
    if (CheatsInputForAction(MenuAction::Toggle) ||
        CheatsInputForAction(MenuAction::Left) != CheatsInput::PreviousCategory ||
        CheatsInputForAction(MenuAction::PreviousTab) != CheatsInput::PreviousCategory ||
        CheatsInputForAction(MenuAction::Right) != CheatsInput::NextCategory ||
        CheatsInputForAction(MenuAction::Confirm) != CheatsInput::Toggle ||
        CheatsInputForAction(MenuAction::Alternate) != CheatsInput::ToggleAll)
        std::abort();

    auto check = [](bool ok) { if (!ok) std::abort(); };
    constexpr std::size_t rows = 3;

    // Tabs follow the categories that have cheats; the page lands on the first.
    Model model{CheatCatalog{{Set("Online graphics", 2), Set("Online cheats", 5), Set("Mine", 0)}}};
    CheatsPage page{CheatCategory::Mine, 9, 9};
    ClampCheatsPage(page, model.catalog, rows);
    check(page.category == CheatCategory::Gameplay && page.selected == 0 && page.first == 0);
    std::array<CheatCategory, CategoryCount> tab_storage;
    const auto tabs = CheatTabs(model.catalog, tab_storage);
    check(tabs.size() == 2 && tabs[0] == CheatCategory::Gameplay && tabs[1] == CheatCategory::Graphics);
    check(tabs.data() == tab_storage.data() && CheatTabCount(model.catalog) == tabs.size());
    check(CheatTabs(CheatCatalog{}, tab_storage).empty());

    // Down scrolls the window with the cursor and wraps to the top.
    for (int i = 0; i < 4; ++i) check(HandleCheatsInput(page, model, CheatsInput::Down, rows) == CheatsEffect::Moved);
    check(page.selected == 4 && page.first == 2);
    check(HandleCheatsInput(page, model, CheatsInput::Down, rows) == CheatsEffect::Moved);
    check(page.selected == 0 && page.first == 0);
    check(HandleCheatsInput(page, model, CheatsInput::Up, rows) == CheatsEffect::Moved);
    check(page.selected == 4 && page.first == 2);

    // A toggles and saves; X turns the whole tab on, then off again.
    check(HandleCheatsInput(page, model, CheatsInput::Toggle, rows) == CheatsEffect::Changed && model.saves == 1);
    check(model.catalog.EnabledCount(CheatCategory::Gameplay) == 1);
    check(HandleCheatsInput(page, model, CheatsInput::ToggleAll, rows) == CheatsEffect::Changed);
    check(model.catalog.EnabledCount(CheatCategory::Gameplay) == 5);
    check(HandleCheatsInput(page, model, CheatsInput::ToggleAll, rows) == CheatsEffect::Changed);
    check(model.catalog.EnabledCount(CheatCategory::Gameplay) == 0);

    // The bumpers switch tabs (wrapping) and start the new tab from its top.
    check(HandleCheatsInput(page, model, CheatsInput::NextCategory, rows) == CheatsEffect::Moved);
    check(page.category == CheatCategory::Graphics && page.selected == 0);
    check(HandleCheatsInput(page, model, CheatsInput::NextCategory, rows) == CheatsEffect::Moved);
    check(page.category == CheatCategory::Gameplay);
    check(HandleCheatsInput(page, model, CheatsInput::PreviousCategory, rows) == CheatsEffect::Moved);
    check(page.category == CheatCategory::Graphics);

    // Refresh and Back are the host's to act on.
    check(HandleCheatsInput(page, model, CheatsInput::Refresh, rows) == CheatsEffect::Refresh);
    check(HandleCheatsInput(page, model, CheatsInput::Back, rows) == CheatsEffect::Back);

    // A cheat that does not fit the VM is refused and says so.
    Model full{CheatCatalog{{Set("Online cheats", 5, 0xF0)}}};
    CheatsPage big;
    check(HandleCheatsInput(big, full, CheatsInput::ToggleAll, rows) == CheatsEffect::OverBudget);
    check(full.catalog.EnabledCount(CheatCategory::Gameplay) == 4 && full.catalog.UsedWords() <= WordBudget);
    big.selected = 4;
    check(HandleCheatsInput(big, full, CheatsInput::Toggle, rows) == CheatsEffect::OverBudget);

    // An empty catalog: nothing moves, nothing toggles.
    Model empty;
    CheatsPage none;
    ClampCheatsPage(none, empty.catalog, rows);
    check(HandleCheatsInput(none, empty, CheatsInput::Down, rows) == CheatsEffect::None);
    check(HandleCheatsInput(none, empty, CheatsInput::Toggle, rows) == CheatsEffect::None);
    check(HandleCheatsInput(none, empty, CheatsInput::NextCategory, rows) == CheatsEffect::None);

    // The game options list opens its option's page and closes with Back.
    GameOptionsPage options{true, 7};
    HandleGameOptionsInput(options, GameOptionsInput::Up);
    check(options.selected == GameOptions().size() - 1); // the list wraps
    HandleGameOptionsInput(options, GameOptionsInput::Down);
    HandleGameOptionsInput(options, GameOptionsInput::Down);
    check(GameOptions()[options.selected] == GameOption::Cheats); // Mods, then Cheats
    HandleGameOptionsInput(options, GameOptionsInput::Confirm);
    check(options.page == GameOption::Cheats && options.open);
    HandleGameOptionsInput(options, GameOptionsInput::Back);
    check(!options.open);

    std::puts("cheats page: tabs, scrolling, toggles, all on/off, budget and game options PASS");
    return 0;
}
