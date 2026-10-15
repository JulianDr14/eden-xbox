// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <string_view>
#include <vector>
#include <utility>
#include "eden_uwp/controller_selection.h"

namespace EdenXbox {
// Owned by the active input loop, on its apartment. No process-global WinRT objects,
// callbacks or locks. Discovery is injected so timing/selection can be tested on host.
template <typename Device>
class ControllerCatalog {
public:
    using Clock = std::chrono::steady_clock;
    template <typename Discover, typename Equal>
    bool Update(Clock::time_point now, Discover discover, Equal equal) {
        if (initialized && now < next_check) return false;
        initialized = true;
        next_check = now + std::chrono::milliseconds{500};
        auto next = discover(); // Preserve the last complete catalog if discovery throws.
        read_failed = false;
        if (equal(devices, next)) return false;
        devices = std::move(next);
        return true;
    }
    const std::vector<Device>& Devices() const { return devices; }
    // Avoid retrying a disconnected/stale WinRT object every input poll.
    void ReadFailed() { read_failed = true; }
    const Device* Select(std::wstring_view preferred) const {
        if (read_failed) return nullptr;
        const auto index = SelectControllerDevice(devices, preferred);
        return index ? &devices[*index] : nullptr;
    }
private:
    std::vector<Device> devices;
    Clock::time_point next_check{};
    bool initialized{}, read_failed{};
};
}
