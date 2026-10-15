// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_library.h"
#include "eden_uwp/stick_navigation.h"
#include "eden_uwp/controller_catalog.h"
#include "eden_uwp/text_entry_input.h"
#include "eden_uwp/uwp_library_canvas.h"
#include "eden_uwp/uwp_rom_storage.h"
#include "eden_uwp/storage_path.h"
#include "eden_uwp/uwp_file_manager.h"

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
#include <winrt/Windows.Storage.Pickers.h>
#include "eden_uwp/cheats/cheat_session.h"
#include "eden_uwp/cheats/cheats_page.h"
#include "eden_uwp/game_library.h"
#include "eden_uwp/game_options_page.h"
#include "eden_uwp/content/content_view.h"
#include "eden_uwp/mods/mod_session.h"
#include "eden_uwp/mods/mod_web.h"
#include "eden_uwp/mods/mods_page.h"
#include "eden_uwp/mods/mods_view.h"
#include "eden_uwp/pulse_sessions.h"
#include "eden_uwp/library_cache.h"
#include "eden_uwp/localization.h"
#include "eden_uwp/system_page.h"
#include "eden_uwp/uwp_input.h"
#include "eden_uwp/uwp_library_metadata.h"
#include "eden_uwp/uwp_system_settings.h"
#include <fmt/format.h>
#include "common/logging.h"
#include "core/file_sys/common_funcs.h"
#include "core/core.h"
#include "core/hle/service/filesystem/filesystem.h"

namespace EdenXbox {
namespace {
using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::GamepadButtons;
using winrt::Windows::System::VirtualKey;
using I18n::Tr;
using T = I18n::Text;

/// Games per row of the grid; the metadata window is the visible page and both neighbours.
constexpr size_t PageSize = 5;
/// Background batches stay short so a page change never waits long behind them: games opened
/// through the loaders per fill batch, cache records read per names batch.
constexpr size_t FillBatch = 6;
constexpr size_t NamesBatch = 64;

std::wstring Wide(std::string_view utf8) {
    return winrt::to_hstring(utf8).c_str();
}
} // namespace

std::optional<std::string> ShowGameLibrary(void* core_window, unsigned width, unsigned height,
                                         const std::filesystem::path& root,
                                         const std::function<void()>& seed,
                                         std::string_view auto_pick, std::string_view initial_notice) {
    using namespace winrt::Windows::UI::Core;
    auto window = CoreWindow::GetForCurrentThread();
    auto canvas = std::make_unique<LibraryCanvas>(core_window, width, height);
    LibraryScan scan;
    size_t selected = 0;
    bool dirty = true, loading = true, settings = false;
    ConfigurationPanel configuration;
    SystemPage system_page;
    // The selected game's options (Menu), and its cheats page while open.
    GameOptionsPage game_options;
    std::unique_ptr<Content::ContentSession> content_session;
    bool missing_content = false;
    std::optional<Cheats::CheatSession> cheat_session;
    Cheats::CheatsPage cheats_page;
    std::unique_ptr<Mods::ModSession> mod_session;
    Mods::ModsPage mods_page;
    std::wstring options_notice;
    system_page.settings = LoadSystemSettings();
    system_page.locale = DetectSystemLocale();
    // Titles are read in the game language, so the library shows what the games will.
    Settings::Language title_language = ApplySystemSettings().game_language;
    const LibraryCache cache{DefaultLibraryCacheDirectory()};
    bool return_to_configuration = false;
    unsigned picker_kind = 0; // 0 games, 1 keys, 2 firmware
    std::future<FileSetupStatus> setup_future;
    std::jthread setup_worker;
    FileImportProgress import_progress;
    std::future<std::string> import_future;
    std::jthread import_worker;
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
    auto system_notice_until = std::chrono::steady_clock::time_point{};
    auto options_notice_until = std::chrono::steady_clock::time_point{};
    // Set by input handlers when the name editor closes: true keeps the name, false discards it.
    std::optional<bool> finish_name_editor;
    auto save_keyboard = [&] {
        const bool saved = SaveKeyboardBindings(panel.keyboard.bindings);
        panel.keyboard.notice = Tr(saved ? T::KeyboardSaved : T::KeyboardSaveFailed);
        keyboard_notice_until = saved ? std::chrono::steady_clock::now() + std::chrono::seconds{3} :
                                       std::chrono::steady_clock::time_point::max();
        dirty = true;
    };
    // The Pulse session each game keeps, as the tiles and the options show it.
    auto refresh_pulse = [&] {
        for (auto& entry : scan.entries) {
            const auto kept = PulseSessions::Find(entry.launch_path);
            entry.pulse_saved = kept ? PulseSessions::FormatTime(kept->saved) : std::wstring{};
            entry.pulse_current = kept && kept->current;
        }
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
    size_t names_cursor = 0; // Next game whose title is looked up in the cache, after a scan.
    size_t fill_cursor = 0;  // Next game the background fill reads into the cache.
    unsigned actions = 0;
    enum : unsigned { Up = 1, Down = 2, Play = 4, Quit = 8, Swap = 16, Zone = 32,
                      Refresh = 64, Panel = 128, Left = 256, Right = 512, AddFolder = 1024,
                      Options = 2048, PreviousTab = 4096, NextTab = 8192 };
    using winrt::Windows::Storage::StorageFolder;
    winrt::Windows::Foundation::IAsyncOperation<StorageFolder> picker{nullptr};
    std::future<std::string> validation;
    std::jthread validation_worker;
    std::string selected_path;
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
    // Typing on a real keyboard while the name editor is open; KeyDown handles the rest.
    auto characters = window.CharacterReceived(winrt::auto_revoke, [&](auto&&, CharacterReceivedEventArgs const& e) {
        if (!system_page.name_editor) return;
        system_page.name_editor->Type(static_cast<wchar_t>(e.KeyCode()));
        system_page.editor_notice.clear();
        dirty = true;
        e.Handled(true);
    });
    auto keyboard = window.KeyDown(winrt::auto_revoke, [&](auto&&, KeyEventArgs const& e) {
        if (e.KeyStatus().WasKeyDown) return;
        if (system_page.name_editor) {
            using Command = TextEntry::Command;
            std::optional<Command> command;
            switch (e.VirtualKey()) {
            case VirtualKey::Escape: command = Command::Cancel; break;
            case VirtualKey::Enter: command = Command::Done; break;
            case VirtualKey::Back: command = Command::Delete; break;
            case VirtualKey::Up: command = Command::Up; break;
            case VirtualKey::Down: command = Command::Down; break;
            case VirtualKey::Left: command = Command::Left; break;
            case VirtualKey::Right: command = Command::Right; break;
            default: break; // Characters arrive through CharacterReceived.
            }
            if (command) {
                const auto result = system_page.name_editor->Apply(*command);
                if (result != TextEntry::Result::None) finish_name_editor = result == TextEntry::Result::Done;
            }
            dirty = true;
            e.Handled(true);
            return;
        }
        auto& editor = panel.keyboard;
        if (editor.open) {
            const auto key = e.VirtualKey();
            if (editor.capturing) {
                if (key != VirtualKey::Escape) {
                    if (AssignKeyboardKey(editor.bindings, editor.selected, static_cast<unsigned>(key))) save_keyboard();
                    else {
                        editor.notice = Tr(T::KeyUnsupported);
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
        case VirtualKey::Y:
        case VirtualKey::R: actions |= Refresh; break;
        case VirtualKey::O: actions |= AddFolder; break;
        case VirtualKey::F2: actions |= Options; break;
        case VirtualKey::Q: actions |= PreviousTab; break;
        case VirtualKey::E: actions |= NextTab; break;
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
        if (system_page.name_editor) {
            using L = SystemPageLayout;
            if (const auto key = TextEntryPanelLayout::KeyAt(x - L::EditorX, y - L::EditorY)) {
                auto& editor = *system_page.name_editor;
                editor.Focus(key->first, key->second);
                if (editor.Press() == TextEntry::Result::Done) finish_name_editor = true;
                system_page.editor_notice.clear();
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        if (system_page.open && configuration.open && !settings) {
            using L = SystemPageLayout;
            if (y >= L::Footer - 7 && y < L::Footer + 40 && x >= L::RowX + L::RowWidth - 230 &&
                x < L::RowX + L::RowWidth) {
                actions |= Quit;
            } else if (x >= L::RowX && x < L::RowX + L::RowWidth && y >= L::RowTop) {
                const auto row = static_cast<size_t>((y - L::RowTop) / L::RowStride);
                if (row < SystemPage::RowCount && y < L::RowTop + row * L::RowStride + L::RowHeight) {
                    system_page.selected = row;
                    actions |= Play;
                    dirty = true;
                }
            }
            e.Handled(true);
            return;
        }
        if (configuration.open && !settings && !content_session) {
            const auto layout = GetConfigurationLayout(configuration);
            if (y >= layout.footer - 7 && y < layout.footer + 40 &&
                x >= layout.x + layout.width - 230 && x < layout.x + layout.width) actions |= Quit;
            else if (!configuration.busy && x >= layout.x && x < layout.x + layout.width && y >= layout.top) {
                const auto row = static_cast<size_t>((y - layout.top) / layout.stride);
                const auto choice = ConfigurationFirstRow(configuration) + row;
                if (row < 5 && y < layout.top + row * layout.stride + layout.row_height &&
                    choice < ConfigurationRowCount(configuration)) {
                    configuration.selected = choice; actions |= Play; dirty = true;
                }
            }
            e.Handled(true);
            return;
        }
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
        } else if (x >= 839 && x < 1064 && y >= 43 && y < 87) actions |= AddFolder;
        else if (x >= 1080 && x < 1226 && y >= 43 && y < 87) actions |= Panel;
        else if (!loading && x >= 320 && x < 504 && y >= 334 && y < 380) actions |= Play;
        else if (!loading && x >= 520 && x < 720 && y >= 334 && y < 380) actions |= Options;
        else if (!loading && x >= 58 && x < 1226 && y >= 444 && y < 635) {
            const size_t index = selected / PageSize * PageSize + static_cast<size_t>((x - 58) / 232);
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
    struct MetadataBatch {
        std::vector<LibraryEntry> entries;
        unsigned generation{};
        bool icons{};      // The window around the cursor.
        bool stopped{};    // Interrupted: some entries were not read.
    };
    bool metadata_background = false; // The running job is a names or fill batch.
    std::future<MetadataBatch> metadata;
    std::jthread metadata_worker;
    unsigned generation = 0;
    bool logging_ready = false;
    auto refresh_setup = [&] {
        if (setup_future.valid()) return;
        std::packaged_task<FileSetupStatus()> task{[] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
            return ReadFileSetupStatus();
        }};
        setup_future = task.get_future();
        setup_worker = std::jthread(std::move(task));
    };
    // Forget what the entries know and read it again (cache first): after the game language
    // changed or the cache was cleared. Results in flight belong to the old generation.
    auto reload_metadata = [&] {
        ++generation;
        metadata_worker.request_stop();
        for (auto& entry : scan.entries) {
            entry.metadata_loaded = entry.title_loaded = false;
            entry.display_version.clear();
            std::vector<unsigned char>{}.swap(entry.icon);
        }
        metadata_dirty = true;
        retained_page = SIZE_MAX;
        names_cursor = fill_cursor = 0;
        dirty = true;
    };
    auto start_scan = [&](StorageFolder added = nullptr) {
        loading = dirty = true;
        ++generation;
        metadata_dirty = true;
        retained_page = SIZE_MAX;
        names_cursor = fill_cursor = 0;
        metadata_worker.request_stop();
        canvas->InvalidateCovers();
        std::packaged_task<LibraryScan(std::stop_token)> task{[&, added](std::stop_token stop) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
            seed();
            // Recover interrupted publication before a metadata worker constructs NCA readers.
            ReadFileSetupStatus();
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            auto result = ScanGameLibrary(root, stop);
            const std::wstring internal{Tr(T::SourceInternal)};
            for (auto& entry : result.entries) {
                const auto path = (root / entry.relative_path).u8string();
                entry.launch_path.assign(reinterpret_cast<const char*>(path.data()), path.size());
                entry.source_name = internal;
            }
            try {
                if (added) RememberGameFolder(added);
                auto external = ScanExternalGameFolders(stop);
                if (!external.error.empty()) {
                    if (!result.error.empty()) result.error += " | ";
                    result.error += external.error;
                }
                result.limited |= external.limited;
                result.entries.insert(result.entries.end(),
                    std::make_move_iterator(external.entries.begin()), std::make_move_iterator(external.entries.end()));
            } catch (const winrt::hresult_error& e) {
                result.error = I18n::ToUtf8(I18n::Format(T::FolderAuthFailed, {e.message()}));
            } catch (const std::exception& e) { result.error = e.what(); }
            std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
                return a.launch_path < b.launch_path;
            });
            return result;
        }};
        future = task.get_future();
        scanner = std::jthread(std::move(task));
    };
    // Saves and applies the System page; the interface switches language on the next frame.
    auto apply_system_effect = [&](SystemPageEffect effect, std::chrono::steady_clock::time_point now) {
        switch (effect) {
        case SystemPageEffect::Changed:
        case SystemPageEffect::NameDone: {
            const bool saved = SaveSystemSettings(system_page.settings);
            const auto resolved = ApplySystemSettings();
            system_page.notice = Tr(saved ? T::SettingsSaved : T::SystemSaveFailed);
            system_notice_until = saved ? now + std::chrono::seconds{3} : std::chrono::steady_clock::time_point::max();
            if (resolved.game_language != title_language) {
                title_language = resolved.game_language;
                reload_metadata();
            }
            break;
        }
        case SystemPageEffect::ClearCache: {
            reload_metadata();
            canvas->InvalidateCovers();
            // The mods store's pictures, index and unfinished downloads go with it.
            std::error_code ec;
            std::filesystem::remove_all(Mods::Web::CacheFolder(), ec);
            system_page.notice = Tr(cache.Clear() ? T::LibraryCacheCleared : T::SystemSaveFailed);
            system_notice_until = now + std::chrono::seconds{3};
            break;
        }
        case SystemPageEffect::None:
        case SystemPageEffect::EditName:
            break;
        }
        dirty = true;
    };
    start_scan();
    GamepadButtons previous{};
    StickNavigation stick_navigation;
    unsigned navigation_context = 0;
    std::optional<ControllerDevice> pad;
    ControllerCatalog<ControllerDevice> controllers;
    while (true) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        const auto now = std::chrono::steady_clock::now();
        if (!panel.keyboard.notice.empty() && now >= keyboard_notice_until) {
            panel.keyboard.notice.clear(); dirty = true;
        }
        if (!panel.notice.empty() && now >= controller_notice_until) {
            panel.notice.clear(); dirty = true;
        }
        if (!system_page.notice.empty() && now >= system_notice_until) {
            system_page.notice.clear(); dirty = true;
        }
        if (!options_notice.empty() && now >= options_notice_until) {
            options_notice.clear(); dirty = true;
        }
        if (panel.keyboard.capturing && now >= capture_until) {
            panel.keyboard.capturing = false; dirty = true;
        }
        const bool catalog_changed = controllers.Update(now, [] {
            RefreshProControllers();
            return EnumerateControllers();
        }, SameControllerList);
        if (catalog_changed) {
            panel.devices = controllers.Devices();
            panel.choice = std::min(panel.choice, panel.devices.size() + 1);
            // An open list must not apply a stale row after hotplug/reorder.
            if (panel.expanded) {
                panel.expanded = false;
                actions &= ~Play;
            }
            dirty = true;
        }
        // Selection follows settings immediately; discovery remains rate limited.
        const auto next = controllers.Select(options.controller_id);
        if (bool(pad) != bool(next) || (pad && next &&
            (pad->id != next->id || pad->pad != next->pad || pad->pro != next->pro))) {
            previous = {}; stick_navigation.Reset();
        }
        if (catalog_changed || !pad || !next || pad->id != next->id)
            pad = next ? std::optional<ControllerDevice>{*next} : std::nullopt;
        const unsigned context = game_options.open ? (cheat_session ? 8 : mod_session ? 9 : 7) :
            system_page.name_editor ? 6 : system_page.open ? 5 :
            panel.keyboard.open ? 3 : panel.expanded ? 2 : settings ? 1 : configuration.open ? 4 : 0;
        if (context != navigation_context) {
            stick_navigation.Reset(); navigation_context = context;
        }
        if (pad) {
            try {
                const auto reading = ReadController(*pad).gamepad;
                const auto buttons = reading.Buttons;
                // Track physical edges before mapping: changing the option while holding a
                // button must not synthesize a second press of its new logical counterpart.
                const auto edges = MapFaceButtons(buttons & ~previous,
                    SwapControllerFaceButtons(*pad, options.swap_face_buttons));
                previous = buttons;
                auto hit = [&](GamepadButtons b) { return (edges & b) == b; };
                // The d-pad takes precedence; do not combine opposite directions.
                constexpr auto dpad = GamepadButtons::DPadUp | GamepadButtons::DPadDown |
                                      GamepadButtons::DPadLeft | GamepadButtons::DPadRight;
                unsigned directions = 0;
                if ((buttons & dpad) != GamepadButtons::None) stick_navigation.Reset();
                else {
                    switch (stick_navigation.Update(reading.LeftThumbstickX, reading.LeftThumbstickY, now)) {
                    case StickDirection::Up: directions |= Up; break;
                    case StickDirection::Down: directions |= Down; break;
                    case StickDirection::Left: directions |= Left; break;
                    case StickDirection::Right: directions |= Right; break;
                    case StickDirection::None: break;
                    }
                }
                if (hit(GamepadButtons::DPadUp)) directions |= Up;
                if (hit(GamepadButtons::DPadDown)) directions |= Down;
                if (hit(GamepadButtons::DPadLeft)) directions |= Left;
                if (hit(GamepadButtons::DPadRight)) directions |= Right;
                if (system_page.name_editor) {
                    // Like the console's keyboard: A types, X deletes, Y adds a space, Menu (or RB)
                    // is done, View (or LB) is Shift and B leaves without saving. The bumpers are
                    // what a game's keyboard uses, where + and - belong to the game menu.
                    auto& editor = *system_page.name_editor;
                    const std::pair<bool, MenuAction> presses[] = {
                        {(directions & Up) != 0, MenuAction::Up},
                        {(directions & Down) != 0, MenuAction::Down},
                        {(directions & Left) != 0, MenuAction::Left},
                        {(directions & Right) != 0, MenuAction::Right},
                        {hit(GamepadButtons::A), MenuAction::Confirm},
                        {hit(GamepadButtons::X), MenuAction::Alternate},
                        {hit(GamepadButtons::Y), MenuAction::Refresh},
                        {hit(GamepadButtons::View) || hit(GamepadButtons::LeftShoulder), MenuAction::PreviousTab},
                        {hit(GamepadButtons::Menu) || hit(GamepadButtons::RightShoulder), MenuAction::NextTab},
                        {hit(GamepadButtons::B), MenuAction::Back},
                    };
                    for (const auto& [pressed, action] : presses) {
                        if (!pressed || finish_name_editor) continue;
                        const auto result = editor.Apply(*TextEntryCommand(action));
                        if (result != TextEntry::Result::None) finish_name_editor = result == TextEntry::Result::Done;
                    }
                    if (directions || edges != GamepadButtons::None) {
                        system_page.editor_notice.clear();
                        dirty = true;
                    }
                } else {
                    // Back must remain available while editing/capturing keyboard input.
                    // Track all edges, but suppress other pad actions inside that modal.
                    if (hit(GamepadButtons::B)) actions |= Quit;
                    if (!panel.keyboard.open) {
                        actions |= directions;
                        if (hit(GamepadButtons::A)) actions |= Play;
                        if (hit(GamepadButtons::X)) {
                            if (settings || content_session || missing_content) actions |= Swap;
                            else if (!configuration.open) actions |= AddFolder;
                        }
                        if (hit(GamepadButtons::Y)) actions |= Refresh;
                        if (hit(GamepadButtons::Menu)) actions |= Options;
                        if (hit(GamepadButtons::View)) actions |= Panel;
                        if (hit(GamepadButtons::LeftShoulder)) actions |= PreviousTab;
                        if (hit(GamepadButtons::RightShoulder)) actions |= NextTab;
                    }
                }
            } catch (...) {
                controllers.ReadFailed();
                pad.reset(); previous = {}; stick_navigation.Reset();
            }
        }
        if (window_closed || QuitRequested()) return std::nullopt;
        if (finish_name_editor) {
            apply_system_effect(FinishNameEditor(system_page, *finish_name_editor), now);
            finish_name_editor.reset();
            dirty = true;
        }
        if (picker) {
            actions = 0; // The system picker owns input until it returns.
            if (picker.Status() != winrt::Windows::Foundation::AsyncStatus::Started) {
                try {
                    auto folder = picker.GetResults();
                    if (folder && picker_kind == 0) start_scan(folder);
                    else if (folder && picker_kind == 3 && content_session) content_session->AuthorizeFolder(folder);
                    else if (folder && picker_kind == 4 && mod_session) mod_session->AddFolder(folder);
                    else if (picker_kind >= 3) {} // A page's folder whose page closed meanwhile.
                    else if (folder) {
                        configuration.busy = true;
                        configuration.notice.clear();
                        import_progress.completed = 0;
                        import_progress.total = 0;
                        metadata_worker.request_stop();
                        std::packaged_task<std::string(std::stop_token)> task{
                            [folder, kind = picker_kind, old_metadata = std::move(metadata_worker), &import_progress](std::stop_token stop) mutable {
                                if (old_metadata.joinable()) old_metadata.join();
                                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                                struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                                return ImportSystemFiles(folder, kind == 1 ? FileImport::Keys : FileImport::Firmware, import_progress, stop);
                            }};
                        import_future = task.get_future();
                        import_worker = std::jthread(std::move(task));
                    } else { configuration.notice = Tr(T::SelectionCancelled); dirty = true; }
                } catch (const winrt::hresult_error& e) {
                    configuration.notice = I18n::Format(T::FolderPickFailed, {e.message()}); dirty = true;
                }
                picker = nullptr;
                previous = {};
                stick_navigation.Reset();
            }
        }
        if (missing_content) {
            if (actions & Quit) { missing_content = false; actions = 0; }
            else if (actions & Swap) { Content::AllowMissingForNextLaunch(selected_path); return selected_path; }
            else if (actions & Play) missing_content = false;
            else actions = 0;
            dirty = true;
        }
        if (validation.valid()) {
            actions = 0;
            if (validation.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
                try {
                    const auto error = validation.get();
                    if (error.empty()) return selected_path;
                    if (error == "content-missing") missing_content = true;
                    else notice = Wide(error);
                } catch (const std::exception& e) { notice = Wide(e.what()); }
                dirty = true;
            }
        }
        if (setup_future.valid() && setup_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try { configuration.status = setup_future.get(); }
            catch (...) { configuration.notice = Tr(T::SetupCheckFailed); }
            configuration.selected = std::min(configuration.selected, ConfigurationRowCount(configuration) - 1);
            dirty = true;
        }
        if (configuration.busy) {
            if (actions & Quit) { import_worker.request_stop(); configuration.notice = Tr(T::CancellingImport); }
            actions = 0;
            const auto done = import_progress.completed.load(std::memory_order_relaxed);
            const auto total = import_progress.total.load(std::memory_order_relaxed);
            if (done != configuration.completed || total != configuration.total) {
                configuration.completed = done; configuration.total = total; dirty = true;
            }
            if (import_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
                try {
                    const auto error = import_future.get();
                    configuration.notice = error.empty() ? std::wstring{Tr(T::ImportDone)} : Wide(error);
                } catch (...) { configuration.notice = Tr(T::ImportFailedKept); }
                configuration.busy = false;
                // The old metadata worker has joined; discard results before rescanning.
                metadata = {};
                refresh_setup();
                start_scan();
                dirty = true;
            }
        }
        if (system_page.open && configuration.open && !settings) {
            // The name editor takes its input in the handlers above; the page waits behind it.
            if (!system_page.name_editor && actions) {
                const auto send = [&](unsigned bit, SystemInput input) {
                    if (actions & bit) apply_system_effect(HandleSystemInput(system_page, input), now);
                };
                send(Up, SystemInput::Up);
                send(Down, SystemInput::Down);
                send(Left, SystemInput::Left);
                send(Right, SystemInput::Right);
                send(Play, SystemInput::Confirm);
                send(Quit, SystemInput::Back);
                if (!system_page.open) configuration.selected = HomeSystem;
            }
            actions = 0;
        }
        if (actions & Options && !game_options.open && !settings && !configuration.open &&
            !panel.keyboard.open && !loading && !picker && !validation.valid() && !scan.entries.empty()) {
            game_options = {true, selected};
            options_notice.clear();
            actions = 0;
            dirty = true;
        }
        if (game_options.open) {
            // A rescan may have replaced the list under the page.
            if (!content_session && game_options.game >= scan.entries.size()) {
                game_options = {};
                cheat_session.reset();
                mod_session.reset();
            }
            if (cheat_session && cheat_session->Poll()) {
                Cheats::ClampCheatsPage(cheats_page, cheat_session->Catalog(), Cheats::CheatsLayout::VisibleRows);
                dirty = true;
            }
            const auto notify = [&](T text) {
                options_notice = Tr(text);
                options_notice_until = now + std::chrono::seconds{4};
            };
            if (content_session) {
                dirty |= content_session->Poll();
                if (content_session->Busy()) dirty = true;
                const auto send = [&](unsigned bits, Content::Input input) {
                    if ((actions & bits) && content_session && content_session->Handle(input)) {
                        const bool changed = content_session->Changed();
                        const bool overview = configuration.open;
                        content_session.reset(); game_options.page.reset();
                        if (overview) game_options = {};
                        if (changed) {
                            cache.Clear(); metadata = {}; ++generation;
                            for (auto& e : scan.entries) { e.title_loaded = false; e.metadata_loaded = false; e.build_id = {}; e.display_version.clear(); }
                            metadata_dirty = true; names_cursor = fill_cursor = 0;
                        }
                    }
                };
                send(Up, Content::Input::Up); send(Down, Content::Input::Down);
                send(Play, Content::Input::Confirm); send(Swap | AddFolder, Content::Input::Import);
                send(PreviousTab | NextTab, Content::Input::Storage); send(Refresh, Content::Input::Refresh);
                send(Quit, Content::Input::Back);
                if (content_session && content_session->TakeAuthorizeRequest() && !picker) {
                    try {
                        winrt::Windows::Storage::Pickers::FolderPicker folder_picker;
                        folder_picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                        folder_picker.FileTypeFilter().Append(L"*"); picker_kind = 3;
                        picker = folder_picker.PickSingleFolderAsync();
                    } catch (...) { options_notice = Tr(T::PickerOpenFailed); }
                }
                dirty |= actions != 0;
            } else if (mod_session) {
                using Kind = Mods::ModsCommand::Kind;
                using Mods::ModJob;
                const ModJob finished = mod_session->Poll();
                if (finished != ModJob::None) {
                    const auto& result = mod_session->View();
                    const bool online = result.origin.kind == Mods::SourceKind::GameBanana ||
                                        result.origin.kind == Mods::SourceKind::Cnx;
                    if (result.error != Mods::ModError::None) notify(Mods::ErrorText(result.error));
                    if (finished == ModJob::Reading && result.error == Mods::ModError::None)
                        Mods::ShowVariants(mods_page, result.candidates.size());
                    // A download with several mods in it: the player picks, wherever they are.
                    if (finished == ModJob::Installing && result.error == Mods::ModError::None &&
                        result.installed == 0 && !result.candidates.empty())
                        Mods::ShowVariants(mods_page, result.candidates.size());
                    if (finished == ModJob::Installing && result.installed > 0) {
                        // From a folder, back to the list where the new mods sit on top; from the
                        // store, the player stays where they browse.
                        if (!online) Mods::ShowInstalled(mods_page);
                        if (result.error == Mods::ModError::None) {
                            options_notice = result.updated ? std::wstring{Tr(T::ModsUpdated)}
                                                            : I18n::Plural(T::ModsInstalledOne, T::ModsInstalledOther,
                                                                           result.installed);
                            options_notice_until = now + std::chrono::seconds{4};
                        }
                    }
                    if (finished == ModJob::Browsing) mods_page.selected = mods_page.first = 0;
                    if (mods_page.tab != Mods::ModsTab::Explore)
                        Mods::ClampModsList(mods_page, mods_page.tab == Mods::ModsTab::Installed ? result.mods.size()
                                                                                                : result.files.size());
                    dirty = true;
                }
                if (mod_session->Tick()) dirty = true;
                const auto failed = mod_session->TakeNotice();
                if (failed) notify(Mods::ErrorText(*failed));
                // The log keeps lines in memory until an error: a job's end or a failure writes
                // them out, so the file pulled from the console tells what happened.
                if (finished != ModJob::None || failed) Common::Log::Stop();
                if (mod_session->Busy()) dirty = true; // The progress moves.
                const std::wstring mods_game = scan.entries[game_options.game].name;
                const auto send = [&](unsigned bits, Mods::ModsInput input) {
                    if (!(actions & bits) || !mod_session) return;
                    const auto counts = Mods::CountsFor(*mod_session, mods_page, mods_game);
                    const auto command = Mods::HandleModsInput(mods_page, counts, input);
                    switch (command.kind) {
                    case Kind::Toggle:
                        if (!mod_session->Toggle(command.index)) notify(T::ModsSaveFailed);
                        break;
                    case Kind::Move:
                        if (!mod_session->Move(command.index, command.to)) notify(T::ModsSaveFailed);
                        break;
                    case Kind::Remove: mod_session->Remove(command.index); break;
                    case Kind::Open: options_notice.clear(); mod_session->Open(command.index); break;
                    case Kind::Up: mod_session->Up(); break;
                    case Kind::Install:
                        mod_session->Install(mods_page.chosen);
                        mods_page.screen = Mods::ModsScreen::List;
                        break;
                    case Kind::Dismiss: mod_session->Dismiss(); break;
                    case Kind::Cancel: mod_session->Cancel(); break;
                    case Kind::Store: mod_session->OpenStore(); break;
                    case Kind::Retry: mod_session->RetryStore(); break;
                    case Kind::Want: mod_session->Want(command.index); break;
                    case Kind::Detail: mod_session->OpenDetail(command.index); break;
                    case Kind::Update: mod_session->OpenUpdate(command.index); break;
                    case Kind::CloseDetail: mod_session->CloseDetail(); break;
                    case Kind::Download:
                        if (mod_session->Download(command.index)) {
                            const auto& detail = mod_session->Store().detail;
                            if (detail && command.index < detail->files.size()) {
                                options_notice = I18n::Format(T::ModsDownloadQueued,
                                                              {Wide(detail->files[command.index].name)});
                                options_notice_until = now + std::chrono::seconds{4};
                            }
                        } else {
                            notify(T::ModsDownloadRefused);
                        }
                        break;
                    case Kind::Sort: mod_session->CycleSort(); break;
                    case Kind::Search: mod_session->Search(I18n::ToUtf8(command.text)); break;
                    case Kind::Source: mod_session->SwitchSource(); break;
                    case Kind::FindGame: mod_session->FindGame(I18n::ToUtf8(command.text)); break;
                    case Kind::PickGame: mod_session->PickGame(command.index); break;
                    case Kind::CancelDownloads: mod_session->CancelDownloads(); break;
                    case Kind::Categories: mod_session->OpenCategories(); break;
                    case Kind::EnterCategory: mod_session->EnterCategory(command.index); break;
                    case Kind::LeaveCategory: mod_session->LeaveCategory(); break;
                    case Kind::PickCategory: mod_session->PickCategory(command.index); break;
                    case Kind::Close:
                        mod_session.reset();
                        game_options.page.reset();
                        break;
                    case Kind::None:
                    case Kind::Moved:
                        break;
                    }
                };
                send(Up, Mods::ModsInput::Up);
                send(Down, Mods::ModsInput::Down);
                send(Left, Mods::ModsInput::Left);
                send(Right, Mods::ModsInput::Right);
                send(PreviousTab, Mods::ModsInput::PreviousTab);
                send(NextTab, Mods::ModsInput::NextTab);
                send(Play, Mods::ModsInput::Confirm);
                send(Swap | AddFolder, Mods::ModsInput::Alternate);
                send(Refresh, Mods::ModsInput::Menu);
                send(Options, Mods::ModsInput::Start);
                send(Quit, Mods::ModsInput::Back);
                if (mod_session && mod_session->TakeFolderRequest() && !picker) {
                    try {
                        winrt::Windows::Storage::Pickers::FolderPicker folder_picker;
                        folder_picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                        folder_picker.FileTypeFilter().Append(L"*");
                        picker_kind = 4;
                        picker = folder_picker.PickSingleFolderAsync();
                    } catch (...) { options_notice = Tr(T::PickerOpenFailed); }
                }
                dirty |= actions != 0;
            } else if (cheat_session && actions) {
                using Cheats::CheatsEffect;
                const auto send = [&](unsigned bits, MenuAction action) {
                    if (!(actions & bits)) return;
                    const auto input = Cheats::CheatsInputForAction(action);
                    if (!input) return;
                    switch (Cheats::HandleCheatsInput(cheats_page, *cheat_session, *input,
                                                      Cheats::CheatsLayout::VisibleRows)) {
                    case CheatsEffect::Changed:
                        if (!cheat_session->Saved()) notify(T::CheatsSaveFailed);
                        else options_notice.clear();
                        break;
                    case CheatsEffect::OverBudget: notify(T::CheatsOverBudget); break;
                    case CheatsEffect::Refresh: cheat_session->Retry(); break;
                    case CheatsEffect::Back:
                        cheat_session.reset();
                        game_options.page.reset();
                        break;
                    case CheatsEffect::None:
                    case CheatsEffect::Moved:
                        break;
                    }
                };
                send(Up, MenuAction::Up);
                send(Down, MenuAction::Down);
                send(Left | PreviousTab, MenuAction::PreviousTab);
                send(Right | NextTab, MenuAction::NextTab);
                send(Play, MenuAction::Confirm);
                send(Swap | AddFolder, MenuAction::Alternate);
                send(Refresh, MenuAction::Refresh);
                if (cheat_session) send(Quit, MenuAction::Back);
                dirty = true;
            } else if (actions) {
                const auto send = [&](unsigned bit, GameOptionsInput input) {
                    if (actions & bit) HandleGameOptionsInput(game_options, input);
                };
                send(Up, GameOptionsInput::Up);
                send(Down, GameOptionsInput::Down);
                send(Play, GameOptionsInput::Confirm);
                send(Quit, GameOptionsInput::Back);
                if (game_options.confirm_discard || game_options.discard) {
                    auto& game = scan.entries[game_options.game];
                    if (game.pulse_saved.empty()) {
                        game_options.confirm_discard = game_options.discard = false;
                        notify(T::PulseNone);
                    } else if (std::exchange(game_options.discard, false)) {
                        notify(PulseSessions::Discard(game.launch_path) ? T::PulseDiscarded : T::PulseDiscardFailed);
                        refresh_pulse();
                    } else {
                        options_notice = Tr(T::PulseDiscardConfirm);
                        options_notice_until = std::chrono::steady_clock::time_point::max();
                    }
                } else if (options_notice == Tr(T::PulseDiscardConfirm)) {
                    options_notice.clear();
                }
                if (game_options.page == GameOption::Updates || game_options.page == GameOption::Dlc) {
                    const auto& game = scan.entries[game_options.game];
                    if (!game.title_loaded || !game.program_id) {
                        notify(T::CheatsUnavailable); game_options.page.reset();
                    } else {
                        std::vector<std::uint64_t> bases;
                        for (const auto& e : scan.entries) if (e.program_id) bases.push_back(FileSys::GetBaseTitleID(e.program_id));
                        metadata_worker.request_stop();
                        content_session = std::make_unique<Content::ContentSession>(game.program_id, game.launch_path,
                            *game_options.page == GameOption::Updates ? Content::Kind::Update : Content::Kind::Dlc,
                            std::move(bases), std::move(metadata_worker));
                        metadata = {};
                    }
                }
                if (game_options.page == GameOption::Mods) {
                    // Mods are kept per title id; the build id (of the version that boots, update
                    // included) tells which mods' patches fit it.
                    const auto& game = scan.entries[game_options.game];
                    if (game.title_loaded && game.program_id == 0) {
                        notify(T::ModsHomebrew);
                        game_options.page.reset();
                    } else if (!game.title_loaded) {
                        notify(T::CheatsUnavailable);
                        game_options.page.reset();
                    } else {
                        std::string build_id;
                        if (std::any_of(game.build_id.begin(), game.build_id.end(), [](std::uint8_t b) { return b != 0; }))
                            for (const auto b : game.build_id) build_id += fmt::format("{:02X}", b);
                        // The store and the installs log what went wrong; the log opens with the page.
                        if (!logging_ready) { Common::Log::Initialize(); logging_ready = true; }
                        mod_session = std::make_unique<Mods::ModSession>(game.program_id, std::move(build_id),
                                                                         game.launch_path, I18n::ToUtf8(game.name),
                                                                         system_page.settings.show_adult_mods);
                        mods_page = {};
                        options_notice.clear();
                    }
                }
                if (game_options.page == GameOption::Cheats) {
                    // Cheat files are named after the game's title and main build ids, which the
                    // library reads with the title (uwp_library_metadata.cpp).
                    const auto& game = scan.entries[game_options.game];
                    const bool known = std::any_of(game.build_id.begin(), game.build_id.end(),
                                                   [](std::uint8_t b) { return b != 0; });
                    if (game.title_loaded && game.program_id == 0) {
                        notify(T::CheatsHomebrew);
                        game_options.page.reset();
                    } else if (!game.title_loaded || !known) {
                        notify(T::CheatsUnavailable);
                        game_options.page.reset();
                    } else {
                        Cheats::CheatGame cheat_game{game.program_id};
                        std::copy(game.build_id.begin(), game.build_id.end(), cheat_game.build_id.begin());
                        cheat_session.emplace(cheat_game);
                        cheats_page = {};
                        options_notice.clear();
                    }
                }
                dirty = true;
            }
            actions = 0;
        }
        if (actions & Quit) {
            if (panel.keyboard.open) { panel.keyboard.open = panel.keyboard.capturing = false; dirty = true; actions &= ~Quit; }
            else if (panel.expanded) { panel.expanded = false; dirty = true; actions &= ~Quit; }
            else if (settings) { settings = false; configuration.open = return_to_configuration; return_to_configuration = false; dirty = true; actions &= ~Quit; }
            else if (configuration.open) {
                if (configuration.files) { configuration.files = false; configuration.selected = HomeFiles; }
                else configuration.open = false;
                dirty = true; actions &= ~Quit;
            }
            else return std::nullopt;
        }
        if (actions & Panel && !configuration.open) { settings = !settings; panel.keyboard.open = panel.keyboard.capturing = false; panel.expanded = false; dirty = true; }
        if (actions & AddFolder && !settings && !picker && !configuration.busy && !validation.valid()) {
            configuration.open = true; configuration.files = false; configuration.selected = HomeSystem;
            configuration.notice.clear(); refresh_setup(); dirty = true;
        }
        if (configuration.open && !settings && !picker && !configuration.busy) {
            const auto count = ConfigurationRowCount(configuration);
            if (actions & Up) configuration.selected = (configuration.selected + count - 1) % count;
            if (actions & Down) configuration.selected = (configuration.selected + 1) % count;
            if (actions & (Up | Down)) dirty = true;
            if (actions & Play) {
                if (!configuration.files) {
                    switch (configuration.selected) {
                    case HomeFiles: configuration.files = true; configuration.selected = 0; break;
                    case HomeControllers: settings = true; return_to_configuration = true; break;
                    default:
                        system_page.open = true;
                        system_page.selected = SystemPage::UserName;
                        system_page.notice.clear();
                        // Windows' language or region may have changed since the library opened.
                        system_page.locale = DetectSystemLocale();
                        break;
                    }
                } else if (loading || setup_future.valid()) configuration.notice = Tr(T::WaitForCheck);
                else if (configuration.selected == 3 || configuration.selected == 4) {
                    std::vector<std::uint64_t> bases;
                    for (const auto& e : scan.entries) if (e.program_id) bases.push_back(FileSys::GetBaseTitleID(e.program_id));
                    metadata_worker.request_stop();
                    content_session = std::make_unique<Content::ContentSession>(0, "",
                        configuration.selected == 3 ? Content::Kind::Update : Content::Kind::Dlc,
                        std::move(bases), std::move(metadata_worker));
                    metadata = {}; game_options.open = true; game_options.game = 0;
                }
                else if (configuration.selected >= 5) {
                    try {
                        ForgetGameFolder(configuration.status.sources[configuration.selected - 5].token);
                        configuration.notice = Tr(T::FolderRemoved);
                        configuration.selected = 0; refresh_setup(); start_scan();
                    } catch (...) { configuration.notice = Tr(T::FolderRemoveFailed); }
                } else {
                    picker_kind = static_cast<unsigned>(configuration.selected);
                    try {
                        winrt::Windows::Storage::Pickers::FolderPicker folder_picker;
                        folder_picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                        folder_picker.FileTypeFilter().Append(L"*");
                        picker = folder_picker.PickSingleFolderAsync();
                        configuration.notice = Tr(picker_kind == 0 ? T::PickGamesFolder :
                                                  picker_kind == 1 ? T::PickKeysFolder : T::PickFirmwareFolder);
                    } catch (...) { configuration.notice = Tr(T::PickerOpenFailed); }
                }
                dirty = true;
            }
            actions = 0;
        }
        if (!settings) panel.notice.clear();
        if (!panel.keyboard.open) panel.keyboard.notice.clear();
        if (loading && future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try { scan = future.get(); }
            catch (const std::exception& e) { scan.error = e.what(); }
            catch (const winrt::hresult_error& e) { scan.error = winrt::to_string(e.message()); }
            loading = false;
            refresh_pulse();
            if (!configuration.busy) refresh_setup();
            selected = std::min(selected, scan.entries.empty() ? 0 : scan.entries.size() - 1);
            notice = Wide(scan.error);
            if (notice.empty() && !initial_notice.empty()) notice = Wide(initial_notice);
            initial_notice = {};
            if (scan.limited) notice = Tr(T::ScanLimit);
            dirty = true;
        }
        if (!auto_pick.empty() && !configuration.open && !loading && !picker && !validation.valid()) {
            if (auto_pick_at == std::chrono::steady_clock::time_point{})
                auto_pick_at = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            if (std::chrono::steady_clock::now() >= auto_pick_at) {
                for (size_t i = 0; i < scan.entries.size(); ++i) {
                    const auto path = scan.entries[i].relative_path.u8string();
                    if (scan.entries[i].launch_path == auto_pick ||
                        (!IsStoragePath(scan.entries[i].launch_path) &&
                         std::string_view{reinterpret_cast<const char*>(path.data()), path.size()} == auto_pick)) {
                        selected = i; settings = false; actions |= Play;
                    }
                }
            }
        }
        if (actions & Play && !configuration.open && !settings && !panel.keyboard.open && !loading && !scan.entries.empty()) {
            selected_path = scan.entries[selected].launch_path;
            notice = Tr(T::CheckingGame);
            dirty = true;
            metadata_worker.request_stop();
            metadata = {};
            std::packaged_task<std::string()> task{[path = selected_path, old_metadata = std::move(metadata_worker)]() mutable {
                if (old_metadata.joinable()) old_metadata.join();
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                if (!HasUsableHeaderKey() && !path.ends_with(".nro"))
                    return I18n::ToUtf8(Tr(T::MissingKeys));
                if (IsStoragePath(path)) {
                    if (const auto error = CheckExternalGame(path); !error.empty()) return error;
                }
                if (path.ends_with(".nro")) return std::string{};
                Core::System system;
                system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
                system.SetFilesystem(MakeUwpFilesystem());
                system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
                FileSys::ManualContentProvider provider;
                const auto resolved = Content::ResolveLaunchContent(*system.GetFilesystem(), path, provider, false, {}, Content::ReportedSystemVersion(system));
                if (resolved.error == Content::Error::Unavailable) return std::string{"content-missing"};
                return resolved.error == Content::Error::None ? std::string{} : I18n::ToUtf8(Tr(Content::ErrorText(resolved.error)));
            }};
            validation = task.get_future();
            validation_worker = std::jthread(std::move(task));
            actions = 0;
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
            if (actions & Up && selected >= PageSize) { selected -= PageSize; dirty = true; }
            if (actions & Down && selected + PageSize < scan.entries.size()) { selected += PageSize; dirty = true; }
        }
        if (actions & Swap) { options.swap_face_buttons = !options.swap_face_buttons; dirty = true; }
        if (actions & Zone) {
            options.deadzone = options.deadzone < 0.10f ? 0.12f : options.deadzone < 0.15f ? 0.18f : 0.08f;
            dirty = true;
        }
        if (device_changed || actions & (Swap | Zone)) {
            device_changed = false;
            const bool saved = SaveControllerOptions(options);
            panel.notice = Tr(saved ? T::SettingsSaved : T::ControllerSaveFailed);
            controller_notice_until = saved ? now + std::chrono::seconds{3} :
                                             std::chrono::steady_clock::time_point::max();
        }
        if (actions & Refresh && !loading && !picker && !validation.valid()) start_scan();
        if (!configuration.busy && metadata.valid() && metadata.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try {
                auto batch = metadata.get();
                if (batch.generation == generation) {
                    // Merge field by field: a names-only batch must not drop icons already shown.
                    const size_t page = selected / PageSize * PageSize;
                    const size_t window_first = page >= PageSize ? page - PageSize : 0;
                    for (auto& entry : batch.entries) {
                        auto found = std::find_if(scan.entries.begin(), scan.entries.end(), [&](const auto& e) {
                            return e.launch_path == entry.launch_path;
                        });
                        if (found == scan.entries.end()) continue;
                        const auto index = static_cast<size_t>(found - scan.entries.begin());
                        // An interrupted background batch is resumed from its first unread game.
                        if (batch.stopped && !batch.icons && !entry.title_loaded) {
                            names_cursor = std::min(names_cursor, index);
                            fill_cursor = std::min(fill_cursor, index);
                        }
                        found->name = std::move(entry.name);
                        found->developer = std::move(entry.developer);
                        if (entry.title_loaded) found->display_version = std::move(entry.display_version);
                        if (entry.title_loaded) {
                            found->program_id = entry.program_id;
                            found->build_id = entry.build_id;
                        }
                        found->title_loaded |= entry.title_loaded;
                        // The player may have paged on meanwhile: keep icons only inside the window.
                        if (batch.icons && index >= window_first && index < page + 2 * PageSize)
                            found->icon = std::move(entry.icon);
                    }
                    dirty = true;
                }
            } catch (const std::exception&) { notice = Tr(T::MetadataFailed); }
        }
        // Metadata, one worker at a time and in priority order: the covers around the cursor, then
        // every title the cache already has, then the rest of the library into the cache.
        const size_t page = selected / PageSize * PageSize;
        // The player moved to another page: background work yields to its covers.
        if (metadata.valid() && metadata_background && (metadata_dirty || retained_page != page))
            metadata_worker.request_stop();
        if (!content_session && !validation.valid() && !missing_content && !configuration.open && !loading && !metadata.valid() && !scan.entries.empty()) {
            MetadataOptions request{title_language};
            std::vector<LibraryEntry> requested;
            if (metadata_dirty || retained_page != page) {
                metadata_dirty = false;
                retained_page = page;
                const size_t window_first = page >= PageSize ? page - PageSize : 0;
                const size_t window_end = std::min(page + 2 * PageSize, scan.entries.size());
                for (size_t i = 0; i < scan.entries.size(); ++i) {
                    auto& entry = scan.entries[i];
                    if ((i < window_first || i >= window_end) && entry.metadata_loaded) {
                        std::vector<unsigned char>{}.swap(entry.icon);
                        entry.metadata_loaded = false;
                    }
                }
                // The visible page first, then the next one (where the player usually goes), then the previous.
                const auto want = [&](size_t first, size_t last) {
                    for (size_t i = first; i < std::min(last, scan.entries.size()); ++i) {
                        auto& entry = scan.entries[i];
                        if (entry.metadata_loaded) continue;
                        entry.metadata_loaded = true;
                        requested.push_back(entry);
                    }
                };
                want(page, page + PageSize);
                want(page + PageSize, page + 2 * PageSize);
                want(window_first, page);
                request.icons = true;
            } else {
                // Titles without icons: what the grid and the cache need, never a cover's bytes.
                const auto take = [&](size_t& cursor, size_t batch) {
                    while (cursor < scan.entries.size() && requested.size() < batch) {
                        const auto& entry = scan.entries[cursor++];
                        if (!entry.title_loaded)
                            requested.push_back({entry.relative_path, entry.name, {}, {}, false, false,
                                                 entry.launch_path, entry.source_name});
                    }
                };
                if (names_cursor < scan.entries.size()) {
                    take(names_cursor, NamesBatch);
                    request.cache_only = true;
                } else {
                    take(fill_cursor, FillBatch);
                }
            }
            if (!requested.empty()) {
                if (!logging_ready) { Common::Log::Initialize(); logging_ready = true; }
                std::packaged_task<MetadataBatch(std::stop_token)> task{
                    [root, &cache, requested = std::move(requested), generation, request](std::stop_token stop) mutable {
                        winrt::init_apartment(winrt::apartment_type::multi_threaded);
                        struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                        ReadLibraryMetadata(root, requested, cache, request, stop);
                        return MetadataBatch{std::move(requested), generation, request.icons,
                                             stop.stop_requested()};
                    }};
                metadata_background = !request.icons;
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
        if (dirty) {
            const GameOptionsView options_view{
                game_options, game_options.open && game_options.game < scan.entries.size() ? &scan.entries[game_options.game] : nullptr,
                cheat_session ? &*cheat_session : nullptr, cheats_page, options_notice, content_session.get(),
                mod_session.get(), &mods_page};
            canvas->Draw({scan, selected, loading, settings, setting_row, options, notice, panel,
                          configuration, system_page, options_view, missing_content});
            dirty = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
}
} // namespace EdenXbox
