// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "eden_uwp/controller_style.h"
#include "eden_uwp/ui_actions.h"
#include "eden_uwp/localization.h"
#include "eden_uwp/ui_text.h"
#include "eden_uwp/upscaling.h"

namespace EdenXbox {
// In-game menu: View + Menu held for a second (Escape on a keyboard) pauses the game and opens it.
// The guide button belongs to the system, so the shortcut has to come from game buttons: Switch -
// and +, which almost no game holds together.

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
    /// PC only: the window can switch to full screen from the menu (the Series always is).
    bool can_fullscreen{};
    bool fullscreen{};
    bool show_performance_stats{};
    /// Pulse sessions are on: leaving for home keeps the game where it is, unless the player
    /// turns that off on the row (pulse_sessions.h).
    bool pulse_sessions{};
    /// Shown live: the paused frame is presented again with each change.
    Upscaling upscaling{Upscaling::Bilinear};
    /// Opt-in diagnostic package: capture from a controller without a keyboard.
    bool material_capture{};
};

/// Library: back to home, keeping a Pulse session when they are on. LibraryWithoutSession: the
/// player chose to leave without one.
enum class GameMenuResult : std::uint8_t {
    None, Resume, Library, SettingsChanged, FullScreen, Cheats, PerformanceStatsChanged, UpscalingChanged,
    LibraryWithoutSession,
    CaptureGraphics,
};

/// One row as drawn: a label, and the value of a setting (changed with left/right) or of a page.
struct GameMenuRow {
    enum class Kind : std::uint8_t { Action, Choice, Page };
    std::wstring label;
    std::wstring value;
    Kind kind{Kind::Action};
};

/// The menu's items and cursor, with texts in the interface language.
class GameMenu {
public:
    enum Item : std::size_t {
        Continue, Cheats, Mods, Style, FaceButtons, Deadzone, FullScreen, ImageUpscaling, PerformanceStats, Library,
        CaptureGraphics,
    };

    /// `cheats`: the game can have cheats (it has a title id), so the menu offers them.
    explicit GameMenu(GameMenuSettings settings_, bool cheats = false) : settings{settings_} {
        items = {Continue};
        if (cheats) items.push_back(Cheats);
        items.insert(items.end(), {Style, FaceButtons, Deadzone});
        if (settings.can_fullscreen) items.push_back(FullScreen);
        items.push_back(ImageUpscaling);
        items.push_back(PerformanceStats);
        if (settings.material_capture) items.push_back(CaptureGraphics);
        items.push_back(Library);
    }

    /// How many cheats are on, shown on the Cheats row.
    void SetCheatsOn(std::size_t count) { cheats_on = count; }

    /// How many mods are on: with any, a row says so. It only informs; mods change in the library,
    /// and a game reads them when it boots.
    void SetModsOn(std::size_t count) {
        mods_on = count;
        const auto found = std::find(items.begin(), items.end(), Mods);
        if (count == 0 || found != items.end()) return;
        const auto after = std::find(items.begin(), items.end(), Cheats);
        items.insert(after != items.end() ? after + 1 : items.begin() + 1, Mods);
    }

    GameMenuResult Apply(MenuAction action) {
        switch (action) {
        case MenuAction::Toggle:
        case MenuAction::Back:
            return GameMenuResult::Resume;
        case MenuAction::Up:
            selected = (selected + items.size() - 1) % items.size();
            return GameMenuResult::None;
        case MenuAction::Down:
            selected = (selected + 1) % items.size();
            return GameMenuResult::None;
        case MenuAction::Left:
        case MenuAction::Right:
        case MenuAction::Confirm:
            return Change(action);
        case MenuAction::Alternate:
        case MenuAction::Refresh:
        case MenuAction::PreviousTab:
        case MenuAction::NextTab:
            break;
        }
        return GameMenuResult::None;
    }

    [[nodiscard]] std::vector<GameMenuRow> Rows() const {
        std::vector<GameMenuRow> rows;
        for (const Item item : items) rows.push_back(Row(item));
        return rows;
    }

    /// Row of the cursor, as drawn.
    [[nodiscard]] std::size_t Selected() const { return selected; }
    [[nodiscard]] Item SelectedItem() const { return items[selected]; }
    [[nodiscard]] const GameMenuSettings& Settings() const { return settings; }

private:
    static std::wstring Tr(I18n::Text id) { return std::wstring{I18n::Tr(id)}; }

    GameMenuRow Row(Item item) const {
        using I18n::Text;
        constexpr auto Choice = GameMenuRow::Kind::Choice;
        switch (item) {
        case Continue:
            return {Tr(Text::MenuResume)};
        case CaptureGraphics:
            return {Tr(Text::MenuCaptureGraphics)};
        case Cheats:
            return {Tr(Text::CheatsTitle), I18n::Plural(Text::CheatsOnOne, Text::CheatsOnOther, cheats_on),
                    GameMenuRow::Kind::Page};
        case Mods:
            return {Tr(Text::ModsTitle), I18n::Plural(Text::ModsActiveOne, Text::ModsActiveOther, mods_on) +
                                             L" · " + Tr(Text::MenuModsAtBoot)};
        case Style:
            return {Tr(Text::ControllerSeenAs), Tr(ControllerStyleText(settings.style)), Choice};
        case FaceButtons:
            return {Tr(Text::FaceButtons), Tr(settings.swap_face_buttons ? Text::ByPosition : Text::ByLetter),
                    Choice};
        case Deadzone:
            return {Tr(Text::DeadZone), std::to_wstring(static_cast<int>(settings.deadzone * 100 + 0.5f)) + L"%",
                    Choice};
        case ImageUpscaling:
            return {Tr(Text::Upscaling), Tr(UpscalingText(settings.upscaling)), Choice};
        case PerformanceStats:
            return {Tr(Text::PerformanceStats), Tr(settings.show_performance_stats ? Text::Yes : Text::No), Choice};
        case FullScreen:
            return {Tr(Text::MenuFullScreen), Tr(settings.fullscreen ? Text::Yes : Text::No), Choice};
        case Library:
            if (!settings.pulse_sessions) return {Tr(Text::MenuBackToHome)};
            return {Tr(Text::MenuBackToHome), Tr(keep_session ? Text::MenuKeepSession : Text::MenuSkipSession),
                    Choice};
        }
        return {};
    }

    GameMenuResult Change(MenuAction action) {
        const bool back = action == MenuAction::Left;
        switch (SelectedItem()) {
        case Continue:
            return action == MenuAction::Confirm ? GameMenuResult::Resume : GameMenuResult::None;
        case CaptureGraphics:
            return action == MenuAction::Confirm ? GameMenuResult::CaptureGraphics : GameMenuResult::None;
        case Cheats:
            return action == MenuAction::Confirm ? GameMenuResult::Cheats : GameMenuResult::None;
        case Mods:
            return GameMenuResult::None;
        case Library:
            if (action != MenuAction::Confirm) {
                keep_session = !keep_session;
                return GameMenuResult::None;
            }
            return !settings.pulse_sessions || keep_session ? GameMenuResult::Library
                                                            : GameMenuResult::LibraryWithoutSession;
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
        case ImageUpscaling:
            settings.upscaling = StepUpscaling(settings.upscaling, back);
            return GameMenuResult::UpscalingChanged;
        case PerformanceStats:
            settings.show_performance_stats = !settings.show_performance_stats;
            return GameMenuResult::PerformanceStatsChanged;
        case FullScreen:
            settings.fullscreen = !settings.fullscreen;
            return GameMenuResult::FullScreen;
        }
        return GameMenuResult::None;
    }

    GameMenuSettings settings;
    std::vector<Item> items;
    std::size_t selected{};
    std::size_t cheats_on{};
    std::size_t mods_on{};
    bool keep_session{true};
};
} // namespace EdenXbox
