// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_controllers.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.Collections.h>
#include "common/common_types.h"

namespace EdenXbox {
namespace {
winrt::fire_and_forget ProbeHid(std::function<void(std::string)> diagnostic) {
    using namespace winrt::Windows::Devices::HumanInterfaceDevice;
    using winrt::Windows::Devices::Enumeration::DeviceInformation;
    // Bluetooth pads announce themselves as gamepads (usage 5), USB ones as joysticks (usage 4).
    for (const u16 usage : {u16{4}, u16{5}}) try {
        const auto devices = co_await DeviceInformation::FindAllAsync(
            HidDevice::GetDeviceSelector(1, usage, 0x057e, 0x2009));
        diagnostic("Pro HID gate: usage " + std::to_string(usage) + " devices=" +
                   std::to_string(devices.Size()));
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
            // Every report whose contents change (a 0x30 report's timer byte aside), up to 80,
            // for 20 s: press buttons and move the sticks meanwhile.
            struct Dump {
                std::atomic<unsigned> count{};
                std::atomic<unsigned> logged{};
                std::mutex mutex;
                std::vector<u8> last;
            };
            auto dump = std::make_shared<Dump>();
            const auto token = hid.InputReportReceived([diagnostic, dump](auto&&, const auto& e) {
                dump->count.fetch_add(1);
                try {
                    const auto report = e.Report();
                    const auto data = report.Data();
                    std::vector<u8> bytes(data.data(), data.data() + data.Length());
                    if (bytes.size() > 1 && bytes[0] == 0x30) bytes[1] = 0;
                    std::scoped_lock lock{dump->mutex};
                    if (bytes == dump->last || dump->logged.load() >= 80) return;
                    dump->last = bytes;
                    dump->logged.fetch_add(1);
                    std::string hex;
                    for (size_t i = 0; i < std::min<size_t>(bytes.size(), 16); ++i) {
                        static constexpr char digits[] = "0123456789abcdef";
                        hex += digits[bytes[i] >> 4];
                        hex += digits[bytes[i] & 15];
                        hex += ' ';
                    }
                    diagnostic("Pro HID gate: report=" + std::to_string(report.Id()) + " bytes=" +
                               std::to_string(bytes.size()) + " | " + hex);
                } catch (const winrt::hresult_error&) {}
            });
            co_await winrt::resume_after(std::chrono::seconds{20});
            hid.InputReportReceived(token);
            hid.Close();
            diagnostic("Pro HID gate: received=" + std::to_string(dump->count.load()));
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
    // Windows' own paths (Bluetooth pads never reach the HID reader): what each controller
    // reports while the user presses buttons, logged only when a reading changes.
    try {
        using namespace winrt::Windows::Gaming::Input;
        for (const auto& raw : RawGameController::RawGameControllers()) {
            diagnostic("Pad probe: raw " + winrt::to_string(raw.DisplayName()) + " vid=" +
                       std::to_string(raw.HardwareVendorId()) + " pid=" +
                       std::to_string(raw.HardwareProductId()) + " wireless=" +
                       std::to_string(raw.IsWireless()) + " buttons=" +
                       std::to_string(raw.ButtonCount()) + " axes=" +
                       std::to_string(raw.AxisCount()) + " switches=" +
                       std::to_string(raw.SwitchCount()) + " gamepad=" +
                       std::to_string(bool(Gamepad::FromGameController(raw))));
        }
        std::vector<std::string> last;
        std::string last_raw;
        for (int sample = 0; sample < 600; ++sample) { // 60 s
            // The raw reading of every Nintendo controller: buttons pressed, axes, hat.
            std::string raw_line;
            for (const auto& raw : RawGameController::RawGameControllers()) {
                if (raw.HardwareVendorId() != 0x057e) continue;
                // std::vector<bool> is packed: array_view needs real bools.
                const size_t button_count = raw.ButtonCount();
                std::unique_ptr<bool[]> buttons{new bool[button_count]{}};
                std::vector<GameControllerSwitchPosition> switches(raw.SwitchCount());
                std::vector<double> axes(raw.AxisCount());
                raw.GetCurrentReading(
                    winrt::array_view<bool>(buttons.get(), buttons.get() + button_count),
                    winrt::array_view<GameControllerSwitchPosition>{switches},
                    winrt::array_view<double>{axes});
                raw_line += "raw buttons=";
                for (size_t b = 0; b < button_count; ++b)
                    if (buttons[b]) raw_line += std::to_string(b) + " ";
                raw_line += "axes=";
                for (const double axis : axes) raw_line += std::to_string(static_cast<int>(axis * 100)) + " ";
                raw_line += "hat=";
                for (const auto position : switches) raw_line += std::to_string(static_cast<int>(position)) + " ";
            }
            if (raw_line != last_raw) {
                diagnostic("Pad probe: " + raw_line);
                last_raw = raw_line;
            }
            const auto devices = EnumerateControllers();
            last.resize(devices.size());
            for (size_t i = 0; i < devices.size(); ++i) {
                const auto& device = devices[i];
                const auto r = ReadController(device).gamepad;
                std::string line = winrt::to_string(device.name) + (device.pad ? " gamepad" : " hid") +
                    " ready=" + std::to_string(bool(device)) +
                    " buttons=" + std::to_string(static_cast<unsigned>(r.Buttons)) +
                    " left=" + std::to_string(r.LeftThumbstickX) + "," + std::to_string(r.LeftThumbstickY);
                if (line != last[i]) {
                    diagnostic("Pad probe: " + line);
                    last[i] = std::move(line);
                }
            }
            co_await winrt::resume_after(std::chrono::milliseconds{100});
        }
        diagnostic("Pad probe: done");
    } catch (const winrt::hresult_error& e) {
        diagnostic("Pad probe: HRESULT=" + std::to_string(static_cast<unsigned>(e.code())));
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
            // Over Bluetooth, or over USB before the HID reader has one, Windows itself maps a
            // Nintendo pad as a Gamepad, and by position: its B (bottom) arrives as Xbox A.
            device.nintendo = raw.HardwareVendorId() == 0x057e;
            const bool pro_controller = device.nintendo && raw.HardwareProductId() == 0x2009;
            // The HID reader wins whenever it has the pad, over USB or Bluetooth: Windows' Gamepad
            // for it reads nothing over Bluetooth, and the HID identity is stable across
            // library and gameplay.
            if (pro_controller && !pro.empty()) continue;
            device.name = pro_controller ? L"Nintendo Switch Pro Controller" : raw.DisplayName().c_str();
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
