// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "eden_uwp/controller_style.h"

namespace EdenXbox {
// In-game menu: View + Menu held for a second (Escape on a keyboard) pauses the game and opens it.
// The guide button belongs to the system, so the shortcut has to come from game buttons: Switch -
// and +, which almost no game holds together.

enum class MenuAction : std::uint8_t { Toggle, Up, Down, Left, Right, Confirm, Back };

/// Holds back a lone + or - for a moment, so pressing View and Menu together never reaches the
/// game as a pause press; both held for HoldMs fire Toggle. A lone press is delivered after
/// PendingMs (or at once on release, so taps still arrive).
class MenuComboFilter {
public:
    static constexpr std::uint32_t PendingMs = 100;
    static constexpr std::uint32_t HoldMs = 1000;

    /// Filters this poll's + / - and returns true when the combo fires.
    bool Filter(bool& plus, bool& minus, std::uint32_t now_ms) {
        const bool any = plus || minus;
        if (plus && minus && state != State::Combo) {
            state = State::Combo;
            if (pending_since == NotPending) pending_since = now_ms;
            fired = false;
        }
        switch (state) {
        case State::Idle:
            if (!any) return false;
            state = State::Pending;
            pending_since = now_ms;
            held_plus = plus;
            held_minus = minus;
            plus = minus = false;
            return false;
        case State::Pending:
            if (!any) {
                // Released before the window closed: a tap, delivered now and released next poll.
                state = State::Idle;
                pending_since = NotPending;
                plus = held_plus;
                minus = held_minus;
                return false;
            }
            held_plus = plus;
            held_minus = minus;
            if (now_ms - pending_since < PendingMs) {
                plus = minus = false;
                return false;
            }
            state = State::Passing;
            pending_since = NotPending;
            return false;
        case State::Passing:
            if (!any) state = State::Idle;
            return false;
        case State::Combo: {
            const bool fire = !fired && plus && minus && now_ms - pending_since >= HoldMs;
            if (fire) fired = true;
            if (!any) {
                state = State::Idle;
                pending_since = NotPending;
            }
            plus = minus = false;
            return fire;
        }
        }
        return false;
    }

private:
    static constexpr std::uint32_t NotPending = ~0U;
    enum class State : std::uint8_t { Idle, Pending, Passing, Combo };
    State state{State::Idle};
    std::uint32_t pending_since{NotPending};
    bool held_plus{}, held_minus{}, fired{};
};

struct GameMenuSettings {
    ConsoleControllerStyle style{ConsoleControllerStyle::Auto};
    bool swap_face_buttons{};
    float deadzone{0.12f};
};

enum class GameMenuResult : std::uint8_t { None, Resume, Library, SettingsChanged };

/// The menu's items and cursor. Text is upper-case ASCII: the renderer's overlay font.
class GameMenu {
public:
    enum Item : std::size_t { Continue, Style, FaceButtons, Deadzone, Library, Count };

    explicit GameMenu(GameMenuSettings settings_) : settings{settings_} {}

    GameMenuResult Apply(MenuAction action) {
        switch (action) {
        case MenuAction::Toggle:
        case MenuAction::Back:
            return GameMenuResult::Resume;
        case MenuAction::Up:
            selected = (selected + Count - 1) % Count;
            return GameMenuResult::None;
        case MenuAction::Down:
            selected = (selected + 1) % Count;
            return GameMenuResult::None;
        case MenuAction::Left:
        case MenuAction::Right:
        case MenuAction::Confirm:
            return Change(action);
        }
        return GameMenuResult::None;
    }

    [[nodiscard]] std::vector<std::string> Lines() const {
        return {"CONTINUAR",
                "LA CONSOLA LO VE COMO  < " + StyleName(settings.style) + " >",
                std::string{"BOTONES A B X Y  < "} +
                    (settings.swap_face_buttons ? "POR POSICION" : "POR LETRA") + " >",
                "ZONA MUERTA  < " + std::to_string(static_cast<int>(settings.deadzone * 100 + 0.5f)) +
                    "% >",
                "VOLVER AL INICIO"};
    }

    [[nodiscard]] std::size_t Selected() const { return selected; }
    [[nodiscard]] const GameMenuSettings& Settings() const { return settings; }

    static std::string StyleName(ConsoleControllerStyle style) {
        switch (style) {
        case ConsoleControllerStyle::Pro:         return "PRO CONTROLLER";
        case ConsoleControllerStyle::DualJoycon:  return "JOY-CON DOBLES";
        case ConsoleControllerStyle::Handheld:    return "MODO PORTATIL";
        case ConsoleControllerStyle::LeftJoycon:  return "JOY-CON IZQUIERDO";
        case ConsoleControllerStyle::RightJoycon: return "JOY-CON DERECHO";
        default:                                  return "AUTOMATICO";
        }
    }

private:
    GameMenuResult Change(MenuAction action) {
        const bool back = action == MenuAction::Left;
        switch (selected) {
        case Continue:
            return action == MenuAction::Confirm ? GameMenuResult::Resume : GameMenuResult::None;
        case Library:
            return action == MenuAction::Confirm ? GameMenuResult::Library : GameMenuResult::None;
        case Style:
            settings.style = StepConsoleControllerStyle(settings.style, back);
            return GameMenuResult::SettingsChanged;
        case FaceButtons:
            settings.swap_face_buttons = !settings.swap_face_buttons;
            return GameMenuResult::SettingsChanged;
        case Deadzone: {
            // Same three steps as the library panel.
            constexpr float steps[] = {0.08f, 0.12f, 0.18f};
            std::size_t i = settings.deadzone < 0.10f ? 0 : settings.deadzone < 0.15f ? 1 : 2;
            i = back ? (i + 2) % 3 : (i + 1) % 3;
            settings.deadzone = steps[i];
            return GameMenuResult::SettingsChanged;
        }
        default:
            return GameMenuResult::None;
        }
    }

    GameMenuSettings settings;
    std::size_t selected{Continue};
};
} // namespace EdenXbox
