// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

#include "common/input.h"
#include "common/logging.h"
#include "eden_uwp/uwp_input.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/input_poller.h"

namespace EdenXbox {

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;
using VirtualStick = InputCommon::VirtualGamepad::VirtualStick;
using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::GamepadButtons;

constexpr std::string_view ENGINE = "virtual_gamepad";
constexpr std::size_t PLAYER = 0;
constexpr u32 NUM_BUTTONS = static_cast<u32>(VirtualButton::ButtonCapture) + 1;
constexpr u32 DEFAULT_STEP_MS = 200;
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(4);
/// Below this the stick reads as centered; the rest of the range is rescaled to 0..1.
constexpr float STICK_DEADZONE = 0.12f;
/// Analog triggers count as ZL/ZR pressed past this.
constexpr double TRIGGER_THRESHOLD = 0.5;

constexpr u32 Bit(VirtualButton button) {
    return 1U << static_cast<u32>(button);
}

struct ButtonName {
    std::string_view name;
    VirtualButton button;
};
constexpr std::array BUTTON_NAMES{
    ButtonName{"A", VirtualButton::ButtonA},        ButtonName{"B", VirtualButton::ButtonB},
    ButtonName{"X", VirtualButton::ButtonX},        ButtonName{"Y", VirtualButton::ButtonY},
    ButtonName{"L", VirtualButton::TriggerL},       ButtonName{"R", VirtualButton::TriggerR},
    ButtonName{"ZL", VirtualButton::TriggerZL},     ButtonName{"ZR", VirtualButton::TriggerZR},
    ButtonName{"PLUS", VirtualButton::ButtonPlus},  ButtonName{"MINUS", VirtualButton::ButtonMinus},
    ButtonName{"UP", VirtualButton::ButtonUp},      ButtonName{"DOWN", VirtualButton::ButtonDown},
    ButtonName{"LEFT", VirtualButton::ButtonLeft},  ButtonName{"RIGHT", VirtualButton::ButtonRight},
    ButtonName{"LS", VirtualButton::StickL},        ButtonName{"RS", VirtualButton::StickR},
    ButtonName{"HOME", VirtualButton::ButtonHome},  ButtonName{"CAPTURE", VirtualButton::ButtonCapture},
};

/// Xbox buttons by label: Xbox A is Switch A (on the bottom, where the Switch has B), and so on.
struct GamepadMapping {
    GamepadButtons xbox;
    VirtualButton button;
};
constexpr std::array GAMEPAD_MAPPINGS{
    GamepadMapping{GamepadButtons::A, VirtualButton::ButtonA},
    GamepadMapping{GamepadButtons::B, VirtualButton::ButtonB},
    GamepadMapping{GamepadButtons::X, VirtualButton::ButtonX},
    GamepadMapping{GamepadButtons::Y, VirtualButton::ButtonY},
    GamepadMapping{GamepadButtons::LeftShoulder, VirtualButton::TriggerL},
    GamepadMapping{GamepadButtons::RightShoulder, VirtualButton::TriggerR},
    GamepadMapping{GamepadButtons::LeftThumbstick, VirtualButton::StickL},
    GamepadMapping{GamepadButtons::RightThumbstick, VirtualButton::StickR},
    GamepadMapping{GamepadButtons::Menu, VirtualButton::ButtonPlus},
    GamepadMapping{GamepadButtons::View, VirtualButton::ButtonMinus},
    GamepadMapping{GamepadButtons::DPadUp, VirtualButton::ButtonUp},
    GamepadMapping{GamepadButtons::DPadDown, VirtualButton::ButtonDown},
    GamepadMapping{GamepadButtons::DPadLeft, VirtualButton::ButtonLeft},
    GamepadMapping{GamepadButtons::DPadRight, VirtualButton::ButtonRight},
};

struct PadState {
    u32 buttons{};
    float left_x{}, left_y{}, right_x{}, right_y{};

    bool operator==(const PadState&) const = default;
};

void ApplyDeadzone(double x, double y, float& out_x, float& out_y) {
    const double length = std::hypot(x, y);
    if (length < STICK_DEADZONE) {
        out_x = out_y = 0.0f;
        return;
    }
    const double scale = std::min(1.0, (length - STICK_DEADZONE) / (1.0 - STICK_DEADZONE)) / length;
    out_x = static_cast<float>(x * scale);
    out_y = static_cast<float>(y * scale);
}

PadState ReadGamepad(const Gamepad& pad) {
    const auto reading = pad.GetCurrentReading();
    PadState state{};
    for (const GamepadMapping& mapping : GAMEPAD_MAPPINGS) {
        if ((reading.Buttons & mapping.xbox) == mapping.xbox) {
            state.buttons |= Bit(mapping.button);
        }
    }
    if (reading.LeftTrigger > TRIGGER_THRESHOLD) {
        state.buttons |= Bit(VirtualButton::TriggerZL);
    }
    if (reading.RightTrigger > TRIGGER_THRESHOLD) {
        state.buttons |= Bit(VirtualButton::TriggerZR);
    }
    ApplyDeadzone(reading.LeftThumbstickX, reading.LeftThumbstickY, state.left_x, state.left_y);
    ApplyDeadzone(reading.RightThumbstickX, reading.RightThumbstickY, state.right_x,
                  state.right_y);
    return state;
}

} // namespace

std::optional<InputStep> ParseInputStep(std::string_view spec) {
    InputStep step{.duration_ms = DEFAULT_STEP_MS, .text = std::string{spec}};
    const std::size_t first = spec.find(':');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string seconds{spec.substr(0, first)};
    char* end = nullptr;
    const double at = std::strtod(seconds.c_str(), &end);
    if (end == seconds.c_str() || at < 0.0) {
        return std::nullopt;
    }
    step.at_ms = static_cast<u32>(at * 1000.0);
    std::string_view buttons = spec.substr(first + 1);
    if (const std::size_t second = buttons.find(':'); second != std::string_view::npos) {
        const std::string duration{buttons.substr(second + 1)};
        step.duration_ms = static_cast<u32>(std::strtoul(duration.c_str(), nullptr, 10));
        buttons = buttons.substr(0, second);
    }
    while (!buttons.empty()) {
        const std::size_t plus = buttons.find('+');
        const std::string_view name = buttons.substr(0, plus);
        buttons = plus == std::string_view::npos ? std::string_view{} : buttons.substr(plus + 1);
        if (const auto it = std::ranges::find(BUTTON_NAMES, name, &ButtonName::name);
            it != BUTTON_NAMES.end()) {
            step.buttons |= Bit(it->button);
        } else if (name == "LS_UP") {
            step.left_y = 1.0f;
        } else if (name == "LS_DOWN") {
            step.left_y = -1.0f;
        } else if (name == "LS_LEFT") {
            step.left_x = -1.0f;
        } else if (name == "LS_RIGHT") {
            step.left_x = 1.0f;
        } else if (name == "RS_UP") {
            step.right_y = 1.0f;
        } else if (name == "RS_DOWN") {
            step.right_y = -1.0f;
        } else if (name == "RS_LEFT") {
            step.right_x = -1.0f;
        } else if (name == "RS_RIGHT") {
            step.right_x = 1.0f;
        } else {
            return std::nullopt;
        }
    }
    return step;
}

GamepadInput::GamepadInput(std::vector<InputStep> script_)
    : gamepad{std::make_shared<InputCommon::VirtualGamepad>(std::string{ENGINE})},
      script{std::move(script_)} {
    Common::Input::RegisterInputFactory(std::string{ENGINE},
                                        std::make_shared<InputCommon::InputFactory>(gamepad));
    Common::Input::RegisterOutputFactory(std::string{ENGINE},
                                         std::make_shared<InputCommon::OutputFactory>(gamepad));
}

GamepadInput::~GamepadInput() {
    Stop();
    Common::Input::UnregisterInputFactory(std::string{ENGINE});
    Common::Input::UnregisterOutputFactory(std::string{ENGINE});
}

void GamepadInput::Start() {
    if (!thread.joinable()) {
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }
}

void GamepadInput::Stop() {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
    gamepad->ResetControllers();
}

void GamepadInput::Run(std::stop_token stop) {
    const auto start = std::chrono::steady_clock::now();
    std::optional<Gamepad> pad;
    PadState applied{};
    std::vector<bool> announced(script.size());
    u32 polls = 0;
    while (!stop.stop_requested()) {
        // The gamepad list is cheap to query but changes rarely: look again twice a second.
        if (polls++ % 125 == 0) {
            try {
                const auto pads = Gamepad::Gamepads();
                const bool had_pad = pad.has_value();
                pad = pads.Size() > 0 ? std::optional<Gamepad>{pads.GetAt(0)} : std::nullopt;
                if (pad.has_value() != had_pad) {
                    LOG_INFO(Input, "UWP input: Xbox gamepad {} ({} connected)",
                             pad ? "in use as player 1" : "disconnected", pads.Size());
                }
            } catch (...) {
                pad.reset();
            }
        }
        PadState state{};
        if (pad) {
            try {
                state = ReadGamepad(*pad);
            } catch (...) {
                pad.reset();
            }
        }
        const auto elapsed = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - start)
                                                  .count());
        for (std::size_t index = 0; index < script.size(); ++index) {
            const InputStep& step = script[index];
            if (elapsed < step.at_ms || elapsed >= step.at_ms + step.duration_ms) {
                continue;
            }
            if (!announced[index]) {
                announced[index] = true;
                LOG_INFO(Input, "UWP input: script step {} at {} ms", step.text, elapsed);
            }
            state.buttons |= step.buttons;
            // A scripted stick direction wins over the pad's.
            if (step.left_x != 0.0f || step.left_y != 0.0f) {
                state.left_x = step.left_x;
                state.left_y = step.left_y;
            }
            if (step.right_x != 0.0f || step.right_y != 0.0f) {
                state.right_x = step.right_x;
                state.right_y = step.right_y;
            }
        }
        if (state != applied) {
            for (u32 button = 0; button < NUM_BUTTONS; ++button) {
                const u32 bit = 1U << button;
                if ((state.buttons ^ applied.buttons) & bit) {
                    gamepad->SetButtonState(PLAYER, static_cast<int>(button),
                                            (state.buttons & bit) != 0);
                }
            }
            if (state.left_x != applied.left_x || state.left_y != applied.left_y) {
                gamepad->SetStickPosition(PLAYER, VirtualStick::Left, state.left_x, state.left_y);
            }
            if (state.right_x != applied.right_x || state.right_y != applied.right_y) {
                gamepad->SetStickPosition(PLAYER, VirtualStick::Right, state.right_x,
                                          state.right_y);
            }
            applied = state;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
}

} // namespace EdenXbox
