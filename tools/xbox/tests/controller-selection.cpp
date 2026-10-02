// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "eden_uwp/controller_selection.h"

int main() {
    struct Device { std::wstring_view id; bool pad; explicit operator bool() const { return pad; } };
    std::vector<Device> devices{{L"flight-stick", false}, {L"usb-pad", true}, {L"wireless-pad", true}};
    using EdenXbox::SelectControllerDevice;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    check(SelectControllerDevice(devices, L"") == 1);
    check(SelectControllerDevice(devices, L"wireless-pad") == 2);
    std::reverse(devices.begin(), devices.end());
    check(SelectControllerDevice(devices, L"wireless-pad") == 0);
    devices.erase(devices.begin());
    check(!SelectControllerDevice(devices, L"wireless-pad"));
    check(SelectControllerDevice(devices, L"") == 0);
    check(!SelectControllerDevice(devices, EdenXbox::KeyboardControllerId));
    check(!SelectControllerDevice(devices, L"flight-stick"));
    devices.insert(devices.begin(), {L"wireless-pad", true});
    check(SelectControllerDevice(devices, L"wireless-pad") == 0);
    devices.clear();
    check(!SelectControllerDevice(devices, L""));
    std::puts("controller selection: auto/manual/reorder/disconnect/reconnect/keyboard PASS");
}
