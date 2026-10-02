// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Controller input for the Xbox/UWP frontend. Player 1 (a Pro Controller unless another style is
// chosen, controller_style.h) is fed from two sources:
// - the first Xbox gamepad (Windows.Gaming.Input), mapped by position like a Switch pad;
// - an optional boot.cfg script ("input=<seconds>:<buttons>[:<milliseconds>]"), so unattended
//   runs on the PC and the console press the same buttons at the same times.
// Both drive Eden's "virtual_gamepad" input engine, which hid_core binds to every player.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "eden_uwp/controller_style.h"
#include "eden_uwp/game_menu.h"

namespace Core::HID {
class HIDCore;
}
namespace InputCommon {
class VirtualGamepad;
}

namespace EdenXbox {

/// One step of a boot.cfg input script: hold `buttons` (VirtualButton bits) and the stick
/// directions from `at_ms` after the guest starts, for `duration_ms`.
struct InputStep {
    u32 at_ms{};
    u32 duration_ms{};
    u32 buttons{};
    float left_x{}, left_y{}, right_x{}, right_y{};
    std::string text;
};

/// Parses "<seconds>:<A+B+...>[:<milliseconds>]", e.g. "25:L+R" or "40:LS_RIGHT+B:3000".
/// Buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LS RS HOME CAPTURE; stick directions:
/// LS_UP LS_DOWN LS_LEFT LS_RIGHT and the RS_ ones. Without a duration a step lasts 200 ms.
[[nodiscard]] std::optional<InputStep> ParseInputStep(std::string_view spec);

/// Q on the keyboard: the player asked to close the app (play mode shuts the guest down).
void RequestQuit();
[[nodiscard]] bool QuitRequested();
/// Escape on the keyboard: open or close the in-game menu (game_menu.h).
void RequestGameMenu();

struct ControllerOptions {
    bool swap_face_buttons{}; // Xbox A/B/X/Y -> Switch B/A/Y/X (physical positions).
    float deadzone{0.12f};
    std::wstring controller_id; // Empty=auto; persistent RawGameController ID otherwise.
    ConsoleControllerStyle style{ConsoleControllerStyle::Auto}; // What the guest sees.
};
ControllerOptions LoadControllerOptions();
bool SaveControllerOptions(const ControllerOptions& options);

/// Player 1 / handheld settings for `style` (before HIDCore().ReloadInputDevices()). Handheld
/// connects Eden's handheld controller instead of player 1 and undocks the console, like Eden's
/// own input settings; Auto starts as a Pro Controller.
void ApplyControllerStyleSettings(ConsoleControllerStyle style);

class GamepadInput {
public:
    /// Registers the virtual_gamepad engine; do it before HIDCore().ReloadInputDevices().
    explicit GamepadInput(std::vector<InputStep> script);
    /// Unregisters the engine; call HIDCore().UnloadInputDevices() first.
    ~GamepadInput();

    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;

    /// Starts polling the gamepad and the script clock (call when the guest starts running). In
    /// automatic style it also reconnects player 1 as a controller the game accepts.
    void Start(Core::HID::HIDCore& hid);
    /// Stops polling and releases every input.
    void Stop();

    /// Menu shortcut presses and, while the menu is open, its navigation, oldest first.
    [[nodiscard]] std::vector<MenuAction> TakeMenuActions();
    /// While open, the pad drives the menu and the game sees every button released.
    void SetMenuOpen(bool open);
    [[nodiscard]] ControllerOptions Options();
    /// Applies (and saves) options changed in the menu; a new style reconnects player 1 now.
    void SetOptions(const ControllerOptions& options);

private:
    void Run(std::stop_token stop);
    /// The style the guest currently sees; in Auto, switches to one the game supports first.
    ConsoleControllerStyle SyncControllerStyle();

    std::shared_ptr<InputCommon::VirtualGamepad> gamepad;
    std::vector<InputStep> script;
    std::mutex options_mutex;
    ControllerOptions options;
    std::atomic<bool> style_changed{};
    std::atomic<bool> menu_open{};
    std::mutex menu_mutex;
    std::vector<MenuAction> menu_actions;
    Core::HID::HIDCore* hid{};
    std::jthread thread;
};

} // namespace EdenXbox
