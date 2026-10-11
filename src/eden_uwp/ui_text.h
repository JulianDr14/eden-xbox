// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Interface names for the frontend's enums, so the headers that define them stay free of text.

#include <array>
#include <cstddef>
#include <string_view>

#include "eden_uwp/controller_style.h"
#include "eden_uwp/keyboard_bindings.h"
#include "eden_uwp/localization.h"

namespace EdenXbox {

inline I18n::Text ControllerStyleText(ConsoleControllerStyle style) {
    using I18n::Text;
    switch (style) {
    case ConsoleControllerStyle::Pro:         return Text::StylePro;
    case ConsoleControllerStyle::DualJoycon:  return Text::StyleDualJoycon;
    case ConsoleControllerStyle::Handheld:    return Text::StyleHandheld;
    case ConsoleControllerStyle::LeftJoycon:  return Text::StyleLeftJoycon;
    case ConsoleControllerStyle::RightJoycon: return Text::StyleRightJoycon;
    default:                                  return Text::StyleAuto;
    }
}

/// Button names printed on the controller stay as they are in every language.
inline std::wstring_view KeyboardActionName(std::size_t action) {
    using I18n::Text;
    struct Name {
        std::wstring_view literal;
        Text text;
    };
    static constexpr std::array<Name, KeyboardActionCount> names{{
        {L"A", {}}, {L"B", {}}, {L"X", {}}, {L"Y", {}},
        {{}, Text::ActionLeftStickClick}, {{}, Text::ActionRightStickClick},
        {L"L", {}}, {L"R", {}}, {L"ZL", {}}, {L"ZR", {}}, {L"+", {}}, {L"-", {}},
        {{}, Text::ActionDPadLeft}, {{}, Text::ActionDPadUp},
        {{}, Text::ActionDPadRight}, {{}, Text::ActionDPadDown},
        {L"SL", {}}, {L"SR", {}}, {L"HOME", {}}, {{}, Text::ActionCapture},
        {{}, Text::ActionLeftStickUp}, {{}, Text::ActionLeftStickDown},
        {{}, Text::ActionLeftStickLeft}, {{}, Text::ActionLeftStickRight},
        {{}, Text::ActionRightStickUp}, {{}, Text::ActionRightStickDown},
        {{}, Text::ActionRightStickLeft}, {{}, Text::ActionRightStickRight},
    }};
    if (action >= names.size()) return {};
    return names[action].literal.empty() ? I18n::Tr(names[action].text) : names[action].literal;
}

} // namespace EdenXbox
