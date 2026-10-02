// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
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

    // Menu model.
    GameMenu menu{{ConsoleControllerStyle::Auto, false, 0.12f}};
    check(menu.Lines().size() == GameMenu::Count && menu.Selected() == GameMenu::Continue);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Back) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Toggle) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Up) == GameMenuResult::None && menu.Selected() == GameMenu::Library);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::Library);
    check(menu.Apply(MenuAction::Down) == GameMenuResult::None && menu.Selected() == GameMenu::Continue);
    menu.Apply(MenuAction::Down);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged);
    check(menu.Settings().style == ConsoleControllerStyle::Pro);
    check(menu.Apply(MenuAction::Left) == GameMenuResult::SettingsChanged);
    check(menu.Apply(MenuAction::Left) == GameMenuResult::SettingsChanged);
    check(menu.Settings().style == ConsoleControllerStyle::RightJoycon);
    check(menu.Lines()[GameMenu::Style].find("JOY-CON DERECHO") != std::string::npos);
    menu.Apply(MenuAction::Down);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::SettingsChanged && menu.Settings().swap_face_buttons);
    menu.Apply(MenuAction::Down);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged && menu.Settings().deadzone > 0.15f);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged && menu.Settings().deadzone < 0.10f);
    check(menu.Lines()[GameMenu::Deadzone].find("8%") != std::string::npos);
    // Every line is drawable by the overlay font (upper case, digits, space and a few marks).
    for (const auto& line : menu.Lines())
        for (const char c : line)
            check((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  std::string_view{" <>%-./"}.find(c) != std::string_view::npos);
    std::puts("game menu: combo hold/stagger/tap/held passthrough, navigation and settings PASS");
}
