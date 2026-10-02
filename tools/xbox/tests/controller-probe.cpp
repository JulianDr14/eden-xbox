// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    using namespace winrt::Windows::Gaming::Input;
    try {
        auto initial = RawGameController::RawGameControllers();
        for (int i = 0; i < 30 && RawGameController::RawGameControllers().Size() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        std::printf("RAW CONTROLLERS %u\n", RawGameController::RawGameControllers().Size());
        for (const auto& raw : RawGameController::RawGameControllers()) {
            std::printf("VID=%04x PID=%04x buttons=%u axes=%u switches=%u Gamepad=%d name=%s\n",
                raw.HardwareVendorId(), raw.HardwareProductId(), raw.ButtonCount(), raw.AxisCount(),
                raw.SwitchCount(), bool(Gamepad::FromGameController(raw)), winrt::to_string(raw.DisplayName()).c_str());
            if (raw.ButtonCount() > 64 || raw.AxisCount() > 32 || raw.SwitchCount() > 32) continue;
            std::array<bool, 64> buttons{};
            std::array<double, 32> axes{};
            std::array<GameControllerSwitchPosition, 32> switches{};
            for (unsigned i = 0; i < raw.ButtonCount(); ++i)
                std::printf("button%u label=%d\n", i, static_cast<int>(raw.GetButtonLabel(i)));
            for (unsigned sample = 0; sample < 4; ++sample) {
                auto time = raw.GetCurrentReading(winrt::array_view<bool>(buttons.data(), static_cast<uint32_t>(raw.ButtonCount())),
                    winrt::array_view<GameControllerSwitchPosition>(switches.data(), static_cast<uint32_t>(raw.SwitchCount())),
                    winrt::array_view<double>(axes.data(), static_cast<uint32_t>(raw.AxisCount())));
                std::printf("time=%llu buttons=", time);
                for (unsigned i = 0; i < raw.ButtonCount(); ++i) std::printf("%d", buttons[i]);
                for (unsigned i = 0; i < raw.SwitchCount(); ++i) std::printf(" switch%u=%d", i, static_cast<int>(switches[i]));
                for (unsigned i = 0; i < raw.AxisCount(); ++i) std::printf(" axis%u=%.4f", i, axes[i]);
                std::puts("");
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
        using winrt::Windows::Devices::HumanInterfaceDevice::HidDevice;
        using winrt::Windows::Devices::Enumeration::DeviceInformation;
        const auto selector = HidDevice::GetDeviceSelector(1, 4, 0x057e, 0x2009);
        for (const auto& info : DeviceInformation::FindAllAsync(selector).get()) {
            std::printf("HID ACCESS %d ID %s\n", static_cast<int>(winrt::Windows::Devices::Enumeration::DeviceAccessInformation::CreateFromId(info.Id()).CurrentStatus()),winrt::to_string(info.Id()).c_str());
            const auto hid = HidDevice::FromIdAsync(info.Id(), winrt::Windows::Storage::FileAccessMode::ReadWrite).get();
            std::printf("HID OPEN %d name=%s\n", bool(hid), winrt::to_string(info.Name()).c_str());
            if (!hid) continue;
            std::atomic<unsigned> reports{};
            const auto token = hid.InputReportReceived([&](auto&&, const auto& e) {
                if (reports.fetch_add(1) >= 3) return;
                const auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(e.Report().Data());
                std::printf("HID REPORT %u bytes=%u ", e.Report().Id(), e.Report().Data().Length());
                for (unsigned i = 0; i < 20 && reader.UnconsumedBufferLength(); ++i) std::printf("%02x ", reader.ReadByte());
                std::puts("");
            });
            std::this_thread::sleep_for(std::chrono::seconds{1});
            hid.InputReportReceived(token);
            hid.Close();
            std::printf("HID REPORT COUNT %u\n", reports.load());
        }
    } catch (const winrt::hresult_error& e) {
        std::printf("WinRT error 0x%08x: %s\n", unsigned(e.code()), winrt::to_string(e.message()).c_str());
        return 1;
    }
    winrt::uninit_apartment();
}
