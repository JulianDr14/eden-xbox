// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string_view>
#include <cstddef>

namespace EdenXbox {
inline constexpr std::wstring_view KeyboardControllerId = L"keyboard";
// Shared pure policy: automatic picks a compatible pad; explicit selection
// never silently redirects player input when the chosen device disconnects.
template <typename Devices>
std::optional<size_t> SelectControllerDevice(const Devices& devices, std::wstring_view preferred) {
    if (preferred == KeyboardControllerId) return {};
    for (size_t i = 0; i < devices.size(); ++i) {
        if (static_cast<bool>(devices[i]) && (preferred.empty() || devices[i].id == preferred)) return i;
    }
    return {};
}
}
