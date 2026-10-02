// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Controller input for the Xbox/UWP frontend. Player 1 is a Pro Controller fed from two sources:
// - the first Xbox gamepad (Windows.Gaming.Input), mapped by position like a Switch pad;
// - an optional boot.cfg script ("input=<seconds>:<buttons>[:<milliseconds>]"), so unattended
//   runs on the PC and the console press the same buttons at the same times.
// Both drive Eden's "virtual_gamepad" input engine, which hid_core binds to every player.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/common_types.h"

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

struct ControllerOptions {
    bool swap_face_buttons{}; // Xbox A/B/X/Y -> Switch B/A/Y/X (physical positions).
    float deadzone{0.12f};
    std::wstring controller_id; // Empty=auto; persistent RawGameController ID otherwise.
};
ControllerOptions LoadControllerOptions();
bool SaveControllerOptions(const ControllerOptions& options);

class GamepadInput {
public:
    /// Registers the virtual_gamepad engine; do it before HIDCore().ReloadInputDevices().
    explicit GamepadInput(std::vector<InputStep> script);
    /// Unregisters the engine; call HIDCore().UnloadInputDevices() first.
    ~GamepadInput();

    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;

    /// Starts polling the gamepad and the script clock (call when the guest starts running).
    void Start();
    /// Stops polling and releases every input.
    void Stop();

private:
    void Run(std::stop_token stop);

    std::shared_ptr<InputCommon::VirtualGamepad> gamepad;
    std::vector<InputStep> script;
    ControllerOptions options;
    std::jthread thread;
};

} // namespace EdenXbox
