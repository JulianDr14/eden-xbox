// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_controllers.h"
#include <array>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Profile.h>
#include "input_common/helpers/joycon_protocol/poller.h"

namespace EdenXbox {
namespace {
using namespace InputCommon::Joycon;
using Buttons = winrt::Windows::Gaming::Input::GamepadButtons;
using Hid = winrt::Windows::Devices::HumanInterfaceDevice::HidDevice;
constexpr JoyStickCalibration DefaultCalibration{{0x6cc, 0x6cc, 0x800}, {0x6cc, 0x6cc, 0x800}};
constexpr std::array Mapping{
    std::pair{PadButton::A, Buttons::A}, std::pair{PadButton::B, Buttons::B},
    std::pair{PadButton::X, Buttons::X}, std::pair{PadButton::Y, Buttons::Y},
    std::pair{PadButton::L, Buttons::LeftShoulder}, std::pair{PadButton::R, Buttons::RightShoulder},
    std::pair{PadButton::StickL, Buttons::LeftThumbstick}, std::pair{PadButton::StickR, Buttons::RightThumbstick},
    std::pair{PadButton::Plus, Buttons::Menu}, std::pair{PadButton::Minus, Buttons::View},
    std::pair{PadButton::Up, Buttons::DPadUp}, std::pair{PadButton::Down, Buttons::DPadDown},
    std::pair{PadButton::Left, Buttons::DPadLeft}, std::pair{PadButton::Right, Buttons::DPadRight}};
}

// Read-only mode deliberately avoids output reports: UWP rejects ReadWrite on
// the tested USB interface. Full 0x30 input is decoded by Eden's existing poller.
class ProControllerReader : public std::enable_shared_from_this<ProControllerReader> {
public:
    explicit ProControllerReader(Hid device) : hid{std::move(device)},
        poller{ControllerType::Pro, DefaultCalibration, DefaultCalibration, {}} {
        JoyconCallbacks callbacks{};
        callbacks.on_battery_data = [](auto) {};
        callbacks.on_button_data = [this](int button, bool pressed) {
            const auto native = static_cast<PadButton>(button);
            for (const auto& [source, target] : Mapping) {
                if (source == native) {
                    if (pressed) state.gamepad.Buttons |= target;
                    else state.gamepad.Buttons &= ~target;
                    break;
                }
            }
            if (native == PadButton::ZL) state.gamepad.LeftTrigger = pressed ? 1 : 0;
            if (native == PadButton::ZR) state.gamepad.RightTrigger = pressed ? 1 : 0;
            if (native == PadButton::Home) state.home = pressed;
            if (native == PadButton::Capture) state.capture = pressed;
        };
        callbacks.on_stick_data = [this](int axis, float value) {
            const auto normalized = std::clamp(value, -1.0f, 1.0f);
            switch (static_cast<PadAxes>(axis)) {
            case PadAxes::LeftStickX: state.gamepad.LeftThumbstickX = normalized; break;
            case PadAxes::LeftStickY: state.gamepad.LeftThumbstickY = normalized; break;
            case PadAxes::RightStickX: state.gamepad.RightThumbstickX = normalized; break;
            case PadAxes::RightStickY: state.gamepad.RightThumbstickY = normalized; break;
            case PadAxes::Undefined: break;
            }
        };
        poller.SetCallbacks(callbacks);
    }
    void Start() {
        const auto weak = weak_from_this();
        token = hid.InputReportReceived([weak](auto&&, const auto& args) {
            if (const auto self = weak.lock()) self->Receive(args.Report());
        });
        subscribed = true;
    }
    ~ProControllerReader() {
        try {
            if (subscribed) hid.InputReportReceived(token);
            if (hid) hid.Close();
        } catch (const winrt::hresult_error&) {}
    }
    bool Ready() const {
        std::scoped_lock lock{mutex};
        return std::chrono::steady_clock::now() - received < std::chrono::milliseconds{250};
    }
    ControllerReading Read() const {
        std::scoped_lock lock{mutex};
        return std::chrono::steady_clock::now() - received < std::chrono::milliseconds{250} ? state : ControllerReading{};
    }
private:
    friend bool RunProControllerDecoderSelfTest();
    bool Decode(std::span<u8> bytes) {
        if (bytes.size() < sizeof(InputReportActive) || bytes.size() > 64 || bytes[0] != 0x30) return false;
        std::scoped_lock lock{mutex};
        poller.ReadActiveMode(bytes, {}, {});
        received = std::chrono::steady_clock::now();
        return true;
    }
    void Receive(const winrt::Windows::Devices::HumanInterfaceDevice::HidInputReport& report) {
        try {
            const auto buffer = report.Data();
            // The poller memcpy requires a complete active packet including ID.
            if (report.Id() != 0x30 || buffer.Length() < sizeof(InputReportActive) || buffer.Length() > 64) return;
            std::array<u8, 64> bytes{};
            std::copy_n(buffer.data(), buffer.Length(), bytes.data());
            Decode(std::span<u8>{bytes.data(), buffer.Length()});
        } catch (const winrt::hresult_error&) {} // unplug: stale snapshot becomes neutral
    }
    Hid hid;
    winrt::event_token token{};
    bool subscribed{};
    mutable std::mutex mutex;
    JoyconPoller poller;
    ControllerReading state{};
    std::chrono::steady_clock::time_point received{};
};

bool RunProControllerDecoderSelfTest() {
    ProControllerReader reader{nullptr};
    std::array<u8, 64> packet{};
    packet[0] = 0x30;
    packet[7] = packet[10] = 0x08;
    packet[8] = packet[11] = 0x80;
    const auto neutral = [&] {
        packet[3] = packet[4] = packet[5] = 0;
        return reader.Decode(packet) && reader.Read().gamepad.Buttons == Buttons::None &&
            reader.Read().gamepad.LeftThumbstickX == 0 && reader.Read().gamepad.LeftThumbstickY == 0 &&
            reader.Read().gamepad.RightThumbstickX == 0 && reader.Read().gamepad.RightThumbstickY == 0 &&
            reader.Read().gamepad.LeftTrigger == 0 && reader.Read().gamepad.RightTrigger == 0 &&
            !reader.Read().home && !reader.Read().capture;
    };
    if (!neutral()) return false;
    for (const auto& [source, target] : Mapping) {
        const auto mask = static_cast<unsigned>(source);
        packet[3] = static_cast<u8>(mask >> 8);
        packet[4] = static_cast<u8>(mask >> 16);
        packet[5] = static_cast<u8>(mask);
        if (!reader.Decode(packet) || reader.Read().gamepad.Buttons != target || !neutral()) return false;
    }
    packet[4] = 0x30; packet[5] = 0x80; packet[3] = 0x80;
    if (!reader.Decode(packet) || !reader.Read().home || !reader.Read().capture ||
        reader.Read().gamepad.LeftTrigger != 1 || reader.Read().gamepad.RightTrigger != 1 || !neutral()) return false;
    if (reader.Decode(std::span<u8>{packet.data(), 12})) return false;
    packet[0] = 0x3f;
    if (reader.Decode(packet)) return false;
    return true;
}

namespace {
struct Registry {
    std::mutex mutex;
    std::vector<ControllerDevice> devices;
};
Registry& ProRegistry() { static Registry registry; return registry; }
winrt::fire_and_forget DiscoverPro(bool* busy) {
    try {
        const auto infos = co_await winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(
            Hid::GetDeviceSelector(1, 4, 0x057e, 0x2009));
        auto previous = ProControllers();
        std::vector<ControllerDevice> devices;
        for (const auto& info : infos) {
            const std::wstring id{info.Id().c_str()};
            const auto found = std::find_if(previous.begin(), previous.end(), [&](const auto& d) { return d.id == id; });
            if (found != previous.end() && found->pro && found->pro->Ready()) { devices.push_back(*found); continue; }
            const auto hid = co_await Hid::FromIdAsync(info.Id(), winrt::Windows::Storage::FileAccessMode::Read);
            if (!hid) continue;
            auto pro = std::make_shared<ProControllerReader>(hid);
            pro->Start();
            devices.push_back({id, L"Nintendo Switch Pro Controller", false, nullptr, std::move(pro)});
        }
        auto& registry = ProRegistry();
        // Destroy unsubscribed devices outside the registry lock.
        { std::scoped_lock lock{registry.mutex}; registry.devices.swap(devices); }
    } catch (const winrt::hresult_error&) {}
    *busy = false; // coroutine resumes on the initiating UI apartment
}
}
ControllerDevice::operator bool() const { return pad || pro_ready; }
ControllerReading ReadController(const ControllerDevice& device) {
    if (device.pad) return {device.pad.GetCurrentReading(), false, false};
    return device.pro ? device.pro->Read() : ControllerReading{};
}
std::vector<ControllerDevice> ProControllers() {
    auto& registry = ProRegistry();
    std::scoped_lock lock{registry.mutex};
    auto devices = registry.devices;
    for (auto& device : devices) device.pro_ready = device.pro && device.pro->Ready();
    return devices;
}
void RefreshProControllers() {
    // Called only from the CoreWindow UI loops, including during gameplay.
    static const bool xbox = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
    static bool busy{};
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (xbox || busy || now < next) return;
    next = now + std::chrono::seconds{2};
    busy = true;
    DiscoverPro(&busy);
}
}
