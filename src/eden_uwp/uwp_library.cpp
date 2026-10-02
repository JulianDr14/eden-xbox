// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_library.h"
#include "eden_uwp/stick_navigation.h"
#include "eden_uwp/uwp_library_canvas.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Graphics.Display.h>
#include "eden_uwp/game_library.h"
#include "eden_uwp/uwp_input.h"
#include "eden_uwp/uwp_library_metadata.h"
#include "common/logging.h"

namespace EdenXbox {
namespace {
using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::GamepadButtons;
using winrt::Windows::System::VirtualKey;

} // namespace

std::optional<std::string> ShowGameLibrary(void* core_window, unsigned width, unsigned height,
                                         const std::filesystem::path& root,
                                         const std::function<void()>& seed,
                                         std::string_view auto_pick) {
    using namespace winrt::Windows::UI::Core;
    auto window = CoreWindow::GetForCurrentThread();
    auto canvas = std::make_unique<LibraryCanvas>(core_window, width, height);
    LibraryScan scan;
    size_t selected = 0;
    bool dirty = true, loading = true, settings = false;
    std::chrono::steady_clock::time_point auto_pick_at{};
    unsigned setting_row = 0;
    ControllerPanel panel;
    panel.xbox = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
    ControllerOptions options = LoadControllerOptions();
    if (panel.xbox) options.controller_id.clear();
    panel.keyboard.bindings = LoadKeyboardBindings();
    auto capture_until = std::chrono::steady_clock::time_point{};
    std::wstring notice; // Library scan/metadata messages only.
    auto keyboard_notice_until = std::chrono::steady_clock::time_point{};
    auto controller_notice_until = std::chrono::steady_clock::time_point{};
    auto save_keyboard = [&] {
        const bool saved = SaveKeyboardBindings(panel.keyboard.bindings);
        panel.keyboard.notice = saved ? L"Teclado guardado" : L"No se pudo guardar el teclado";
        keyboard_notice_until = saved ? std::chrono::steady_clock::now() + std::chrono::seconds{3} :
                                       std::chrono::steady_clock::time_point::max();
        dirty = true;
    };
    auto begin_capture = [&] {
        panel.keyboard.notice.clear();
        panel.keyboard.capturing = true;
        capture_until = std::chrono::steady_clock::now() + std::chrono::seconds{4};
        dirty = true;
    };
    bool device_changed = false;
    size_t retained_page = SIZE_MAX;
    bool metadata_dirty = true;
    unsigned actions = 0;
    enum : unsigned { Up = 1, Down = 2, Play = 4, Quit = 8, Swap = 16, Zone = 32,
                      Refresh = 64, Panel = 128, Left = 256, Right = 512 };
    bool resize = false, window_closed = false;
    auto size_changed = window.SizeChanged(winrt::auto_revoke, [&](auto&&, WindowSizeChangedEventArgs const& e) {
        const auto scale = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        width = std::max(1U, static_cast<unsigned>(e.Size().Width * scale));
        height = std::max(1U, static_cast<unsigned>(e.Size().Height * scale));
        resize = true;
    });
    auto closed = window.Closed(winrt::auto_revoke, [&](auto&&, CoreWindowEventArgs const&) { window_closed = true; });
    auto visible = window.VisibilityChanged(winrt::auto_revoke, [&](auto&&, VisibilityChangedEventArgs const&) { dirty = true; });
    auto activation = window.Activated(winrt::auto_revoke, [&](auto&&, WindowActivatedEventArgs const& e) {
        if (e.WindowActivationState() == CoreWindowActivationState::Deactivated) {
            panel.keyboard.capturing = false; dirty = true;
        }
    });
    auto keyboard = window.KeyDown(winrt::auto_revoke, [&](auto&&, KeyEventArgs const& e) {
        if (e.KeyStatus().WasKeyDown) return;
        auto& editor = panel.keyboard;
        if (editor.open) {
            const auto key = e.VirtualKey();
            if (editor.capturing) {
                if (key != VirtualKey::Escape) {
                    if (AssignKeyboardKey(editor.bindings, editor.selected, static_cast<unsigned>(key))) save_keyboard();
                    else {
                        editor.notice = L"Esa tecla no es compatible";
                        keyboard_notice_until = std::chrono::steady_clock::time_point::max();
                    }
                }
                editor.capturing = false;
                dirty = true;
            } else if (key == VirtualKey::Escape) { editor.open = false; dirty = true; }
            else if (key == VirtualKey::Tab) {
                editor.page = (editor.page + 1) % 3; editor.selected = KeyboardPageActions(editor.page).front(); dirty = true;
            } else if (key == VirtualKey::Enter) begin_capture();
            else if (key == VirtualKey::Delete) {
                AssignKeyboardKey(editor.bindings, editor.selected, 0); save_keyboard();
            } else if (key == VirtualKey::Up || key == VirtualKey::Down ||
                       key == VirtualKey::Left || key == VirtualKey::Right) {
                const auto items = KeyboardPageActions(editor.page);
                const size_t count = items.size();
                const size_t index = static_cast<size_t>(std::find(items.begin(), items.end(), editor.selected) - items.begin());
                const size_t step = key == VirtualKey::Up || key == VirtualKey::Down ? 2 : 1;
                const bool reverse = key == VirtualKey::Up || key == VirtualKey::Left;
                editor.selected = items[(index + (reverse ? count - step : step)) % count];
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        switch (e.VirtualKey()) {
        case VirtualKey::Up: actions |= Up; break;
        case VirtualKey::Down: actions |= Down; break;
        case VirtualKey::Left: actions |= Left; break;
        case VirtualKey::Right: actions |= Right; break;
        case VirtualKey::F1: actions |= Panel; break;
        case VirtualKey::Escape: actions |= Quit; break;
        case VirtualKey::Enter: actions |= Play; break;
        case VirtualKey::X: actions |= Swap; break;
        case VirtualKey::Y: actions |= Zone; break;
        case VirtualKey::R: actions |= Refresh; break;
        default: break;
        }
        e.Handled(true);
    });
    auto pointer = window.PointerPressed(winrt::auto_revoke, [&](auto&&, PointerEventArgs const& e) {
        const auto p = e.CurrentPoint().Position();
        const auto dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        const auto scale = std::max(0.01f, std::min(width / 1280.0f, height / 720.0f));
        const float x = static_cast<float>((p.X * dpi - (width - 1280 * scale) / 2) / scale);
        const float y = static_cast<float>((p.Y * dpi - (height - 720 * scale) / 2) / scale);
        if (panel.keyboard.open) {
            auto& editor = panel.keyboard;
            if (Contains({1120, 112, 48, 40}, x, y)) {
                editor.open = editor.capturing = false; dirty = true;
            } else if (!editor.capturing) {
                if (Contains({635, 203, 514, 36}, x, y)) {
                    editor.page = std::min(2U, static_cast<unsigned>((x - 635) / 174));
                    editor.selected = KeyboardPageActions(editor.page).front(); dirty = true;
                } else if (Contains({635, 603, 155, 36}, x, y)) {
                    AssignKeyboardKey(editor.bindings, editor.selected, 0); save_keyboard();
                } else if (Contains({806, 603, 170, 36}, x, y)) {
                    editor.bindings = DefaultKeyboardBindings; save_keyboard();
                } else {
                    for (size_t i = 0; i < KeyboardButtonCount; ++i) {
                        if (Contains(KeyboardDiagramTile(i), x, y)) {
                            editor.page = KeyboardPageFor(i); editor.selected = i; begin_capture(); break;
                        }
                    }
                    for (const size_t i : KeyboardPageActions(editor.page)) {
                        if (!editor.capturing && Contains(KeyboardTileFor(i), x, y)) {
                            editor.selected = i; begin_capture(); break;
                        }
                    }
                }
            }
            e.Handled(true); return;
        }
        if (settings) {
            if (panel.expanded) {
                const size_t first = ControllerChoiceFirst(panel);
                if (x >= 648 && x < 1178 && y >= ControllerChoiceTop) {
                    const size_t i = first + static_cast<size_t>((y - ControllerChoiceTop) / ControllerChoiceHeight);
                    if (i < panel.devices.size() + 2 && i < first + ControllerVisibleChoices) {
                        panel.choice = i;
                        actions |= Play;
                    } else actions |= Quit;
                } else actions |= Quit;
            } else if (!panel.xbox && Contains({648, 510, 530, 34}, x, y)) {
                panel.keyboard.open = true; panel.keyboard.capturing = false; dirty = true;
            } else if (x < 620 || x > 1206 || y < 133 || y > 620) actions |= Panel;
            else if (x >= 648 && x < 1178 && y >= ControllerRowTop &&
                     y < ControllerRowTop + (panel.xbox ? 3 : 4) * ControllerRowHeight) {
                setting_row = static_cast<unsigned>((y - ControllerRowTop) / ControllerRowHeight);
                actions |= Play;
            }
        } else if (x >= 1080 && y >= 43 && y < 87) actions |= Panel;
        else if (!loading && x >= 320 && x < 504 && y >= 334 && y < 380) actions |= Play;
        else if (!loading && x >= 58 && x < 1226 && y >= 444 && y < 635) {
            const size_t index = selected / 5 * 5 + static_cast<size_t>((x - 58) / 232);
            if (index < scan.entries.size()) { selected = index; dirty = true; }
        }
        e.Handled(true);
    });
    auto wheel = window.PointerWheelChanged(winrt::auto_revoke, [&](auto&&, PointerEventArgs const& e) {
        if (settings && panel.expanded && !panel.keyboard.open) {
            const int delta = e.CurrentPoint().Properties().MouseWheelDelta();
            const size_t count = panel.devices.size() + 2;
            if (delta > 0 && panel.choice) --panel.choice;
            if (delta < 0 && panel.choice + 1 < count) ++panel.choice;
            dirty = true;
            e.Handled(true);
        }
    });
    std::future<LibraryScan> future;
    std::jthread scanner;
    struct MetadataBatch { std::vector<LibraryEntry> entries; unsigned generation{}; };
    std::future<MetadataBatch> metadata;
    std::jthread metadata_worker;
    unsigned generation = 0;
    bool logging_ready = false;
    auto start_scan = [&] {
        loading = dirty = true;
        ++generation;
        metadata_dirty = true;
        retained_page = SIZE_MAX;
        metadata_worker.request_stop();
        canvas->InvalidateCovers();
        std::packaged_task<LibraryScan(std::stop_token)> task{[&](std::stop_token stop) {
            seed();
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            return ScanGameLibrary(root, stop);
        }};
        future = task.get_future();
        scanner = std::jthread(std::move(task));
    };
    start_scan();
    GamepadButtons previous{};
    StickNavigation stick_navigation;
    unsigned navigation_context = 0;
    std::optional<ControllerDevice> pad;
    auto next_pad_check = std::chrono::steady_clock::now();
    while (true) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        const auto now = std::chrono::steady_clock::now();
        if (!panel.keyboard.notice.empty() && now >= keyboard_notice_until) {
            panel.keyboard.notice.clear(); dirty = true;
        }
        if (!panel.notice.empty() && now >= controller_notice_until) {
            panel.notice.clear(); dirty = true;
        }
        if (panel.keyboard.capturing && now >= capture_until) {
            panel.keyboard.capturing = false; dirty = true;
        }
        if (now >= next_pad_check) {
            RefreshProControllers();
            auto devices = EnumerateControllers();
            if (!SameControllerList(panel.devices, devices)) {
                panel.devices = std::move(devices);
                panel.choice = std::min(panel.choice, panel.devices.size() + 1);
                // An open list must not apply a stale row after hotplug/reorder.
                if (panel.expanded) {
                    panel.expanded = false;
                    actions &= ~Play;
                }
                dirty = true;
            }
            const auto index = SelectController(panel.devices, options.controller_id);
            const auto next = index ? std::optional<ControllerDevice>{panel.devices[*index]} : std::nullopt;
            if (!pad || !next || pad->id != next->id) {
                previous = {}; stick_navigation.Reset();
            }
            pad = next;
            next_pad_check = now + std::chrono::milliseconds{500};
        }
        const unsigned context = panel.keyboard.open ? 3 : panel.expanded ? 2 : settings ? 1 : 0;
        if (context != navigation_context) {
            stick_navigation.Reset(); navigation_context = context;
        }
        if (pad) {
            try {
                const auto reading = ReadController(*pad).gamepad;
                const auto buttons = reading.Buttons;
                const auto edges = buttons & ~previous;
                previous = buttons;
                auto hit = [&](GamepadButtons b) { return (edges & b) == b; };
                // Back must remain available while editing/capturing keyboard input.
                // Track all edges, but suppress other pad actions inside that modal.
                if (hit(GamepadButtons::B)) actions |= Quit;
                if (!panel.keyboard.open) {
                    // The d-pad takes precedence; do not combine opposite directions.
                    constexpr auto dpad = GamepadButtons::DPadUp | GamepadButtons::DPadDown |
                                          GamepadButtons::DPadLeft | GamepadButtons::DPadRight;
                    if ((buttons & dpad) != GamepadButtons::None) stick_navigation.Reset();
                    else {
                        switch (stick_navigation.Update(reading.LeftThumbstickX, reading.LeftThumbstickY, now)) {
                        case StickDirection::Up: actions |= Up; break;
                        case StickDirection::Down: actions |= Down; break;
                        case StickDirection::Left: actions |= Left; break;
                        case StickDirection::Right: actions |= Right; break;
                        case StickDirection::None: break;
                        }
                    }
                    if (hit(GamepadButtons::DPadUp)) actions |= Up;
                    if (hit(GamepadButtons::DPadDown)) actions |= Down;
                    if (hit(GamepadButtons::DPadLeft)) actions |= Left;
                    if (hit(GamepadButtons::DPadRight)) actions |= Right;
                    if (hit(GamepadButtons::A)) actions |= Play;
                    if (hit(GamepadButtons::X)) actions |= Swap;
                    if (hit(GamepadButtons::Y)) actions |= Zone;
                    if (hit(GamepadButtons::Menu)) actions |= Refresh;
                    if (hit(GamepadButtons::View)) actions |= Panel;
                }
            } catch (...) { pad.reset(); previous = {}; stick_navigation.Reset(); }
        }
        if (window_closed || QuitRequested()) return std::nullopt;
        if (actions & Quit) {
            if (panel.keyboard.open) { panel.keyboard.open = panel.keyboard.capturing = false; dirty = true; actions &= ~Quit; }
            else if (panel.expanded) { panel.expanded = false; dirty = true; actions &= ~Quit; }
            else if (settings) { settings = false; dirty = true; actions &= ~Quit; }
            else return std::nullopt;
        }
        if (actions & Panel) { settings = !settings; panel.keyboard.open = panel.keyboard.capturing = false; panel.expanded = false; dirty = true; }
        if (!settings) panel.notice.clear();
        if (!panel.keyboard.open) panel.keyboard.notice.clear();
        if (loading && future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try { scan = future.get(); }
            catch (const std::exception& e) { scan.error = e.what(); }
            loading = false;
            selected = std::min(selected, scan.entries.empty() ? 0 : scan.entries.size() - 1);
            notice = winrt::to_hstring(scan.error).c_str();
            if (scan.limited) notice = L"Limite de exploracion alcanzado: 10000 entradas, 5 niveles.";
            dirty = true;
        }
        if (!auto_pick.empty() && !loading) {
            if (auto_pick_at == std::chrono::steady_clock::time_point{})
                auto_pick_at = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            if (std::chrono::steady_clock::now() >= auto_pick_at) {
                for (size_t i = 0; i < scan.entries.size(); ++i) {
                    const auto path = scan.entries[i].relative_path.u8string();
                    if (std::string_view{reinterpret_cast<const char*>(path.data()), path.size()} == auto_pick) {
                        selected = i; settings = false; actions |= Play;
                    }
                }
            }
        }
        if (actions & Play && !settings && !panel.keyboard.open && !loading && !scan.entries.empty()) {
            const auto path = scan.entries[selected].relative_path.u8string();
            return std::string{reinterpret_cast<const char*>(path.data()), path.size()};
        }
        if (settings) {
            if (panel.keyboard.open) { actions &= ~(Swap | Zone); }
            else if (panel.expanded) {
                const size_t count = panel.devices.size() + 2;
                if (actions & Up) panel.choice = (panel.choice + count - 1) % count;
                if (actions & Down) panel.choice = (panel.choice + 1) % count;
                if (actions & (Up | Down)) dirty = true;
                if (actions & Play) {
                    if (panel.choice < 2 || (bool(panel.devices[panel.choice - 2]) &&
                        !panel.devices[panel.choice - 2].id.starts_with(L"session:"))) {
                        options.controller_id = panel.choice == 0 ? L"" : panel.choice == 1 ?
                            std::wstring{KeyboardControllerId} : panel.devices[panel.choice - 2].id;
                        device_changed = true;
                        panel.expanded = false;
                        next_pad_check = now;
                    }
                    dirty = true;
                }
                actions &= ~(Swap | Zone);
            } else {
                // Rows: [device (PC)], controller type, A/B/X/Y, deadzone, [keyboard (PC)].
                const unsigned offset = panel.xbox ? 0 : 1;
                const unsigned rows = panel.xbox ? 3 : 5;
                if (actions & Up) setting_row = (setting_row + rows - 1) % rows;
                if (actions & Down) setting_row = (setting_row + 1) % rows;
                if (actions & (Up | Down)) dirty = true;
                if (actions & (Play | Left | Right)) {
                    if (!panel.xbox && setting_row == 4) {
                        panel.keyboard.open = true; panel.keyboard.capturing = false; dirty = true;
                    } else if (!panel.xbox && setting_row == 0) {
                        panel.expanded = true;
                        panel.choice = options.controller_id == KeyboardControllerId ? 1 : 0;
                        for (size_t i = 0; i < panel.devices.size(); ++i)
                            if (panel.devices[i].id == options.controller_id) panel.choice = i + 2;
                        dirty = true;
                    } else if (setting_row == offset) {
                        options.style = StepConsoleControllerStyle(options.style, (actions & Left) != 0);
                        device_changed = true; dirty = true;
                    } else actions |= setting_row == offset + 1 ? Swap : Zone;
                }
            }
        } else {
            actions &= ~(Swap | Zone);
            if (actions & Left && selected) { --selected; dirty = true; }
            if (actions & Right && selected + 1 < scan.entries.size()) { ++selected; dirty = true; }
            if (actions & Up && selected >= 5) { selected -= 5; dirty = true; }
            if (actions & Down && selected + 5 < scan.entries.size()) { selected += 5; dirty = true; }
        }
        if (actions & Swap) { options.swap_face_buttons = !options.swap_face_buttons; dirty = true; }
        if (actions & Zone) {
            options.deadzone = options.deadzone < 0.10f ? 0.12f : options.deadzone < 0.15f ? 0.18f : 0.08f;
            dirty = true;
        }
        if (device_changed || actions & (Swap | Zone)) {
            device_changed = false;
            const bool saved = SaveControllerOptions(options);
            panel.notice = saved ? L"Configuracion guardada" : L"No se pudo guardar el mando";
            controller_notice_until = saved ? now + std::chrono::seconds{3} :
                                             std::chrono::steady_clock::time_point::max();
        }
        if (actions & Refresh && !loading) start_scan();
        if (metadata.valid() && metadata.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try {
                auto batch = metadata.get();
                if (batch.generation == generation) {
                    for (auto& entry : batch.entries) {
                        auto found = std::find_if(scan.entries.begin(), scan.entries.end(), [&](const auto& e) {
                            return e.relative_path == entry.relative_path;
                        });
                        if (found != scan.entries.end()) *found = std::move(entry);
                    }
                    dirty = true;
                    metadata_dirty = true;
                }
            } catch (const std::exception&) { notice = L"No se pudieron leer los datos de algunos juegos."; }
        }
        const size_t page = selected / 5 * 5;
        if (!loading && !metadata.valid() && !scan.entries.empty() &&
            (metadata_dirty || retained_page != page)) {
            metadata_dirty = false;
            std::vector<LibraryEntry> requested;
            const size_t first = selected / 5 * 5;
            if (retained_page != SIZE_MAX && retained_page != first) {
                for (size_t i = retained_page; i < std::min(retained_page + 5, scan.entries.size()); ++i) {
                    auto& entry = scan.entries[i];
                    if (!entry.icon.empty()) {
                        std::vector<unsigned char>{}.swap(entry.icon);
                        entry.metadata_loaded = false;
                    }
                }
            }
            retained_page = first;
            for (size_t i = first; i < std::min(first + 5, scan.entries.size()); ++i) {
                auto& entry = scan.entries[i];
                if (!entry.metadata_loaded) {
                    requested.push_back(entry);
                    entry.metadata_loaded = true;
                }
            }
            if (!requested.empty()) {
                if (!logging_ready) { Common::Log::Initialize(); logging_ready = true; }
                std::packaged_task<MetadataBatch(std::stop_token)> task{
                    [root, requested = std::move(requested), generation](std::stop_token stop) mutable {
                        winrt::init_apartment(winrt::apartment_type::multi_threaded);
                        struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                        ReadLibraryMetadata(root, requested, stop);
                        return MetadataBatch{std::move(requested), generation};
                    }};
                metadata = task.get_future();
                metadata_worker = std::jthread(std::move(task));
            }
        }
        actions = 0;
        if (resize) {
            canvas.reset();
            canvas = std::make_unique<LibraryCanvas>(core_window, width, height);
            resize = false;
            dirty = true;
        }
        if (dirty) { canvas->Draw(scan, selected, loading, settings, setting_row, options, notice, panel); dirty = false; }
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
}
} // namespace EdenXbox
