// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_controllers.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace EdenXbox {
namespace {
winrt::fire_and_forget ProbeHid(std::function<void(std::string)> diagnostic) {
    using namespace winrt::Windows::Devices::HumanInterfaceDevice;
    using winrt::Windows::Devices::Enumeration::DeviceInformation;
    try {
        const auto devices = co_await DeviceInformation::FindAllAsync(
            HidDevice::GetDeviceSelector(1, 4, 0x057e, 0x2009));
        diagnostic("Pro HID gate: devices=" + std::to_string(devices.Size()));
        for (const auto& info : devices) {
            diagnostic("Pro HID gate: access=" + std::to_string(static_cast<int>(
                winrt::Windows::Devices::Enumeration::DeviceAccessInformation::CreateFromId(info.Id()).CurrentStatus())));
            auto hid = co_await HidDevice::FromIdAsync(info.Id(),
                winrt::Windows::Storage::FileAccessMode::ReadWrite);
            diagnostic("Pro HID gate: open=" + std::to_string(bool(hid)));
            if (!hid) {
                hid = co_await HidDevice::FromIdAsync(info.Id(),
                    winrt::Windows::Storage::FileAccessMode::Read);
                diagnostic("Pro HID gate: read-only=" + std::to_string(bool(hid)));
                if (!hid) continue;
            }
            auto count = std::make_shared<std::atomic<unsigned>>(0);
            const auto token = hid.InputReportReceived([diagnostic, count](auto&&, const auto& e) {
                if (count->fetch_add(1) >= 4) return;
                try {
                    const auto report = e.Report();
                    diagnostic("Pro HID gate: report=" + std::to_string(report.Id()) +
                               " bytes=" + std::to_string(report.Data().Length()));
                } catch (const winrt::hresult_error&) {}
            });
            co_await winrt::resume_after(std::chrono::seconds{2});
            hid.InputReportReceived(token);
            hid.Close();
            diagnostic("Pro HID gate: received=" + std::to_string(count->load()));
            for (const auto& device : ProControllers()) {
                const auto reading = ReadController(device);
                diagnostic("Pro HID decoder: ready=" + std::to_string(bool(device)) +
                    " buttons=" + std::to_string(static_cast<unsigned>(reading.gamepad.Buttons)) +
                    " left=" + std::to_string(reading.gamepad.LeftThumbstickX) + "," + std::to_string(reading.gamepad.LeftThumbstickY) +
                    " right=" + std::to_string(reading.gamepad.RightThumbstickX) + "," + std::to_string(reading.gamepad.RightThumbstickY));
            }
        }
    } catch (const winrt::hresult_error& e) {
        diagnostic("Pro HID gate: HRESULT=" + std::to_string(static_cast<unsigned>(e.code())));
    }
}
}
void ProbeProControllerHid(std::function<void(std::string)> diagnostic) {
    ProbeHid(std::move(diagnostic));
}

std::vector<ControllerDevice> EnumerateControllers() {
    using namespace winrt::Windows::Gaming::Input;
    std::vector<ControllerDevice> devices;
    auto pro = ProControllers();
    try {
        for (const auto& raw : RawGameController::RawGameControllers()) {
            ControllerDevice device;
            device.id = raw.NonRoamableId().c_str();
            const bool nintendo_usb = !raw.IsWireless() && raw.HardwareVendorId() == 0x057e && raw.HardwareProductId() == 0x2009;
            if (nintendo_usb && !pro.empty()) continue; // HID identity is stable across library/gameplay.
            device.name = nintendo_usb ? L"Nintendo Switch Pro Controller" : raw.DisplayName().c_str();
            if (device.name.empty()) device.name = L"Mando";
            device.wireless = raw.IsWireless();
            device.pad = Gamepad::FromGameController(raw);
            if (!device.id.empty()) devices.push_back(std::move(device));
        }
    } catch (const winrt::hresult_error&) {
        // Older Xbox contracts may lack raw names/IDs. Retain the existing
        // Gamepad path with explicitly session-scoped fallback identities.
        devices.clear();
    }
    try {
        const auto pads = Gamepad::Gamepads();
        for (unsigned i = 0; i < pads.Size(); ++i) {
            const auto pad = pads.GetAt(i);
            if (std::any_of(devices.begin(), devices.end(), [&](const auto& d) { return d.pad == pad; }))
                continue;
            devices.push_back({L"session:" + std::to_wstring(i),
                               L"Mando " + std::to_wstring(i + 1), pad.IsWireless(), pad});
        }
    } catch (const winrt::hresult_error&) {}
    devices.insert(devices.end(), pro.begin(), pro.end());
    return devices;
}
std::optional<size_t> SelectController(const std::vector<ControllerDevice>& devices,
                                     std::wstring_view preferred) {
    return SelectControllerDevice(devices, preferred);
}
std::wstring ControllerLabel(const std::vector<ControllerDevice>& devices,
                             std::wstring_view preferred) {
    if (preferred == KeyboardControllerId) return L"Teclado";
    if (const auto selected = SelectController(devices, preferred)) return devices[*selected].name;
    return preferred.empty() ? L"Sin mando conectado" : L"Mando elegido desconectado";
}
bool SameControllerList(const std::vector<ControllerDevice>& a,
                        const std::vector<ControllerDevice>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
        return x.id == y.id && x.name == y.name && x.wireless == y.wireless && x.pad == y.pad && x.pro == y.pro && bool(x) == bool(y);
    });
}
}
