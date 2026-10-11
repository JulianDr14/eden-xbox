// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/localization.cpp and src/eden_uwp/locales/*.cpp.
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/game_menu.h"

int main() {
    using namespace EdenXbox;
    auto check = [](bool ok) { if (!ok) std::abort(); };

    // Both pressed together: never reach the game, fire once after a second held.
    {
        MenuComboFilter f;
        int fires = 0;
        for (std::uint32_t t = 0; t <= 1500; t += 4) {
            bool plus = true, minus = true;
            fires += f.Filter(plus, minus, t);
            check(!plus && !minus);
        }
        check(fires == 1);
        bool plus = false, minus = false;
        check(!f.Filter(plus, minus, 1504) && !plus && !minus);
    }
    // View first, Menu 40 ms later: still the combo, - never delivered; released early: no fire.
    {
        MenuComboFilter f;
        bool fired = false;
        for (std::uint32_t t = 0; t < 600; t += 4) {
            bool plus = t >= 40, minus = true;
            fired |= f.Filter(plus, minus, t);
            check(!plus && !minus);
        }
        bool plus = false, minus = false;
        f.Filter(plus, minus, 604);
        check(!fired && !plus && !minus);
    }
    // A quick tap of + reaches the game (on release), then releases.
    {
        MenuComboFilter f;
        bool plus = true, minus = false;
        f.Filter(plus, minus, 0);
        check(!plus);
        plus = true;
        f.Filter(plus, minus, 30);
        check(!plus);
        plus = false;
        f.Filter(plus, minus, 60);
        check(plus && !minus);
        plus = false;
        f.Filter(plus, minus, 64);
        check(!plus);
    }
    // A held + is delivered after the pending window and stays delivered.
    {
        MenuComboFilter f;
        bool delivered = false;
        for (std::uint32_t t = 0; t <= 400; t += 4) {
            bool plus = true, minus = false;
            f.Filter(plus, minus, t);
            if (t >= MenuComboFilter::PendingMs) check(plus);
            else check(!plus);
            delivered |= plus;
        }
        check(delivered);
    }

    // Menu model, in Spanish.
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"es"));
    GameMenu menu{{ConsoleControllerStyle::Auto, false, 0.12f}};
    check(menu.Rows().size() == 5 && menu.SelectedItem() == GameMenu::Continue);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Back) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Toggle) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Up) == GameMenuResult::None && menu.SelectedItem() == GameMenu::Library);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::Library);
    check(menu.Apply(MenuAction::Down) == GameMenuResult::None && menu.SelectedItem() == GameMenu::Continue);
    menu.Apply(MenuAction::Down);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged);
    check(menu.Settings().style == ConsoleControllerStyle::Pro);
    check(menu.Apply(MenuAction::Left) == GameMenuResult::SettingsChanged);
    check(menu.Apply(MenuAction::Left) == GameMenuResult::SettingsChanged);
    check(menu.Settings().style == ConsoleControllerStyle::RightJoycon);
    check(menu.Rows()[1].label == L"La consola lo ve como");
    check(menu.Rows()[1].kind == GameMenuRow::Kind::Choice);
    check(menu.Rows()[1].value.find(L"Joy-Con derecho") != std::wstring::npos);
    menu.Apply(MenuAction::Down);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::SettingsChanged && menu.Settings().swap_face_buttons);
    menu.Apply(MenuAction::Down);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged && menu.Settings().deadzone > 0.15f);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged && menu.Settings().deadzone < 0.10f);
    check(menu.Rows()[3].value == L"8%");
    check(menu.Rows()[0].kind == GameMenuRow::Kind::Action && menu.Rows()[0].value.empty());
    // Presses the menu's own rows ignore.
    for (const auto action : {MenuAction::Alternate, MenuAction::Refresh, MenuAction::PreviousTab,
                              MenuAction::NextTab})
        check(menu.Apply(action) == GameMenuResult::None);

    // A game with a title id offers its cheats, second, with how many are on.
    GameMenu with_cheats{{ConsoleControllerStyle::Auto, false, 0.12f}, true};
    with_cheats.SetCheatsOn(3);
    check(with_cheats.Rows().size() == 6);
    check(with_cheats.Rows()[1].kind == GameMenuRow::Kind::Page && with_cheats.Rows()[1].value == L"3 activos");
    check(with_cheats.Apply(MenuAction::Down) == GameMenuResult::None && with_cheats.SelectedItem() == GameMenu::Cheats);
    check(with_cheats.Apply(MenuAction::Right) == GameMenuResult::None);
    check(with_cheats.Apply(MenuAction::Confirm) == GameMenuResult::Cheats);
    with_cheats.SetCheatsOn(1);
    check(with_cheats.Rows()[1].value == L"1 activo");

    // PC: full screen sits above "back to the library"; the Series menu never shows it.
    GameMenu pc{{ConsoleControllerStyle::Auto, false, 0.12f, true, false}};
    check(pc.Rows().size() == 6 && pc.Rows()[4].label == L"Pantalla completa" && pc.Rows()[4].value == L"No");
    pc.Apply(MenuAction::Up);
    pc.Apply(MenuAction::Up);
    check(pc.SelectedItem() == GameMenu::FullScreen && pc.Selected() == 4);
    check(pc.Apply(MenuAction::Confirm) == GameMenuResult::FullScreen && pc.Settings().fullscreen);
    check(pc.Apply(MenuAction::Right) == GameMenuResult::FullScreen && !pc.Settings().fullscreen);
    for (const auto& row : menu.Rows()) check(row.label != L"Pantalla completa");
    // The same menu follows the interface language.
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"en"));
    check(pc.Rows()[0].label == L"Resume" && pc.Rows()[4].label == L"Full screen" && pc.Rows()[4].value == L"No");
    std::puts("game menu: combo hold/stagger/tap/held passthrough, navigation, settings and cheats PASS");
}
