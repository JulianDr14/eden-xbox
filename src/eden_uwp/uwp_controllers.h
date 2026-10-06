// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/Windows.Gaming.Input.h>
#include "eden_uwp/controller_selection.h"
#include "eden_uwp/keyboard_bindings.h"

namespace EdenXbox {
class ProControllerReader;
struct ControllerReading {
    winrt::Windows::Gaming::Input::GamepadReading gamepad{};
    bool home{}, capture{};
};
struct ControllerDevice {
    std::wstring id;
    std::wstring name;
    bool wireless{};
    winrt::Windows::Gaming::Input::Gamepad pad{nullptr};
    std::shared_ptr<ProControllerReader> pro;
    bool pro_ready{}; // Availability snapshot for UI diffing.
    // Nintendo layout (A right, B bottom). ReadController reports its buttons by their printed
    // letter whichever path reads it, so "by letter" and "by position" are the same on it.
    bool nintendo{};
    explicit operator bool() const;
};
ControllerReading ReadController(const ControllerDevice& device);
// UI-thread discovery; rate limited, asynchronous and PC-only.
void RefreshProControllers();
bool RunProControllerDecoderSelfTest();
std::vector<ControllerDevice> ProControllers();
// Empty preferred ID means auto; KeyboardControllerId explicitly disables pad.
std::vector<ControllerDevice> EnumerateControllers();
// Opt-in transport gate; call on the activated CoreWindow thread.
void ProbeProControllerHid(std::function<void(std::string)> diagnostic);
std::optional<size_t> SelectController(const std::vector<ControllerDevice>& devices,
                                     std::wstring_view preferred);
std::wstring ControllerLabel(const std::vector<ControllerDevice>& devices,
                             std::wstring_view preferred);
bool SameControllerList(const std::vector<ControllerDevice>& a,
                        const std::vector<ControllerDevice>& b);
struct ControllerPanel {
    std::wstring notice;
    KeyboardEditor keyboard;
    bool xbox{};
    bool expanded{};
    size_t choice{};
    std::vector<ControllerDevice> devices;
};
inline constexpr float ControllerRowTop = 258;
inline constexpr float ControllerRowHeight = 62;
inline constexpr float ControllerChoiceTop = 303;
inline constexpr float ControllerChoiceHeight = 42;
inline constexpr size_t ControllerVisibleChoices = 6;
inline size_t ControllerChoiceFirst(const ControllerPanel& panel) {
    return panel.choice / ControllerVisibleChoices * ControllerVisibleChoices;
}
}
