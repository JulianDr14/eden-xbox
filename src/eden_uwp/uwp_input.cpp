// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <mutex>
#include <winrt/Windows.System.h>
#include "eden_uwp/keyboard_bindings.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/main.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>

#include "common/input.h"
#include "common/logging.h"
#include "eden_uwp/uwp_input.h"
#include "eden_uwp/uwp_controllers.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/input_poller.h"

namespace EdenXbox {

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;
using VirtualStick = InputCommon::VirtualGamepad::VirtualStick;
using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::GamepadButtons;

constexpr std::string_view ENGINE = "virtual_gamepad";
constexpr std::size_t PLAYER = 0;
static_assert(static_cast<size_t>(VirtualButton::ButtonCapture) + 1 == KeyboardButtonCount);
constexpr u32 NUM_BUTTONS = static_cast<u32>(VirtualButton::ButtonCapture) + 1;
constexpr u32 DEFAULT_STEP_MS = 200;
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(4);
/// Below this the stick reads as centered; the rest of the range is rescaled to 0..1.
/// Analog triggers count as ZL/ZR pressed past this.
constexpr double TRIGGER_THRESHOLD = 0.5;

constexpr u32 Bit(VirtualButton button) {
    return 1U << static_cast<u32>(button);
}

struct ButtonName {
    std::string_view name;
    VirtualButton button;
};
constexpr std::array BUTTON_NAMES{
    ButtonName{"A", VirtualButton::ButtonA},        ButtonName{"B", VirtualButton::ButtonB},
    ButtonName{"X", VirtualButton::ButtonX},        ButtonName{"Y", VirtualButton::ButtonY},
    ButtonName{"L", VirtualButton::TriggerL},       ButtonName{"R", VirtualButton::TriggerR},
    ButtonName{"ZL", VirtualButton::TriggerZL},     ButtonName{"ZR", VirtualButton::TriggerZR},
    ButtonName{"PLUS", VirtualButton::ButtonPlus},  ButtonName{"MINUS", VirtualButton::ButtonMinus},
    ButtonName{"UP", VirtualButton::ButtonUp},      ButtonName{"DOWN", VirtualButton::ButtonDown},
    ButtonName{"LEFT", VirtualButton::ButtonLeft},  ButtonName{"RIGHT", VirtualButton::ButtonRight},
    ButtonName{"LS", VirtualButton::StickL},        ButtonName{"RS", VirtualButton::StickR},
    ButtonName{"HOME", VirtualButton::ButtonHome},  ButtonName{"CAPTURE", VirtualButton::ButtonCapture},
};

/// Xbox buttons by label: Xbox A is Switch A (on the bottom, where the Switch has B), and so on.
struct GamepadMapping {
    GamepadButtons xbox;
    VirtualButton button;
};
constexpr std::array GAMEPAD_MAPPINGS{
    GamepadMapping{GamepadButtons::A, VirtualButton::ButtonA},
    GamepadMapping{GamepadButtons::B, VirtualButton::ButtonB},
    GamepadMapping{GamepadButtons::X, VirtualButton::ButtonX},
    GamepadMapping{GamepadButtons::Y, VirtualButton::ButtonY},
    GamepadMapping{GamepadButtons::LeftShoulder, VirtualButton::TriggerL},
    GamepadMapping{GamepadButtons::RightShoulder, VirtualButton::TriggerR},
    GamepadMapping{GamepadButtons::LeftThumbstick, VirtualButton::StickL},
    GamepadMapping{GamepadButtons::RightThumbstick, VirtualButton::StickR},
    GamepadMapping{GamepadButtons::Menu, VirtualButton::ButtonPlus},
    GamepadMapping{GamepadButtons::View, VirtualButton::ButtonMinus},
    GamepadMapping{GamepadButtons::DPadUp, VirtualButton::ButtonUp},
    GamepadMapping{GamepadButtons::DPadDown, VirtualButton::ButtonDown},
    GamepadMapping{GamepadButtons::DPadLeft, VirtualButton::ButtonLeft},
    GamepadMapping{GamepadButtons::DPadRight, VirtualButton::ButtonRight},
};

struct PadState {
    u32 buttons{};
    float left_x{}, left_y{}, right_x{}, right_y{};

    bool operator==(const PadState&) const = default;
};

void ApplyDeadzone(double x, double y, float deadzone, float& out_x, float& out_y) {
    const double length = std::hypot(x, y);
    if (length <= deadzone) {
        out_x = out_y = 0.0f;
        return;
    }
    const double scale = std::min(1.0, (length - deadzone) / (1.0 - deadzone)) / length;
    out_x = static_cast<float>(x * scale);
    out_y = static_cast<float>(y * scale);
}

std::atomic<u32> pressed_keys{0};
std::atomic<bool> quit_requested{false};

struct KeyboardRuntime {
    std::mutex mutex;
    std::shared_ptr<InputCommon::Keyboard> engine{std::make_shared<InputCommon::Keyboard>("keyboard")};
    std::vector<std::unique_ptr<Common::Input::InputDevice>> devices;
};
KeyboardRuntime& KeyboardInput() { static KeyboardRuntime runtime; return runtime; }

void ApplyKeyboard(PadState& state) {
    const u32 keys = pressed_keys.load(std::memory_order_relaxed);
    state.buttons |= keys & ((1U << KeyboardButtonCount) - 1);
    auto stick = [&](size_t first, float& x, float& y) {
        const float dx = ((keys >> (first + 3)) & 1) - static_cast<float>((keys >> (first + 2)) & 1);
        const float dy = ((keys >> first) & 1) - static_cast<float>((keys >> (first + 1)) & 1);
        if ((dx || dy) && x == 0 && y == 0) {
            const float scale = dx && dy ? 0.70710678f : 1.0f;
            x = dx * scale; y = dy * scale;
        }
    };
    stick(20, state.left_x, state.left_y);
    stick(24, state.right_x, state.right_y);
}

PadState ReadGamepad(const ControllerDevice& pad, const ControllerOptions& options) {
    const auto full = ReadController(pad);
    const auto& reading = full.gamepad;
    PadState state{};
    for (const GamepadMapping& mapping : GAMEPAD_MAPPINGS) {
        if ((reading.Buttons & mapping.xbox) == mapping.xbox) {
            auto button = mapping.button;
            if (options.swap_face_buttons && pad.pad) {
                switch (button) {
                case VirtualButton::ButtonA: button = VirtualButton::ButtonB; break;
                case VirtualButton::ButtonB: button = VirtualButton::ButtonA; break;
                case VirtualButton::ButtonX: button = VirtualButton::ButtonY; break;
                case VirtualButton::ButtonY: button = VirtualButton::ButtonX; break;
                default: break;
                }
            }
            state.buttons |= Bit(button);
        }
    }
    if (full.home) state.buttons |= Bit(VirtualButton::ButtonHome);
    if (full.capture) state.buttons |= Bit(VirtualButton::ButtonCapture);
    if (reading.LeftTrigger > TRIGGER_THRESHOLD) {
        state.buttons |= Bit(VirtualButton::TriggerZL);
    }
    if (reading.RightTrigger > TRIGGER_THRESHOLD) {
        state.buttons |= Bit(VirtualButton::TriggerZR);
    }
    ApplyDeadzone(reading.LeftThumbstickX, reading.LeftThumbstickY, options.deadzone, state.left_x, state.left_y);
    ApplyDeadzone(reading.RightThumbstickX, reading.RightThumbstickY, options.deadzone, state.right_x,
                  state.right_y);
    return state;
}

} // namespace

ControllerOptions LoadControllerOptions() {
    ControllerOptions options;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(L"controller_device"))
            options.controller_id = winrt::unbox_value<winrt::hstring>(values.Lookup(L"controller_device")).c_str();
        if (values.HasKey(L"controller_swap_face"))
            options.swap_face_buttons = winrt::unbox_value<bool>(values.Lookup(L"controller_swap_face"));
        if (values.HasKey(L"controller_deadzone")) {
            const float zone = static_cast<float>(winrt::unbox_value<double>(values.Lookup(L"controller_deadzone")));
            if (std::isfinite(zone) && zone >= 0.01f && zone <= 0.5f) options.deadzone = zone;
        }
    } catch (...) { /* Missing or invalid settings retain safe defaults. */ }
    // Xbox always uses its default controller, even if settings originated on PC.
    try {
        if (winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox")
            options.controller_id.clear();
    } catch (const winrt::hresult_error&) {}
    return options;
}

bool SaveControllerOptions(const ControllerOptions& options) {
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        values.Insert(L"controller_swap_face", winrt::box_value(options.swap_face_buttons));
        values.Insert(L"controller_device", winrt::box_value(winrt::hstring{options.controller_id}));
        values.Insert(L"controller_deadzone", winrt::box_value(static_cast<double>(options.deadzone)));
        return true;
    } catch (...) { return false; }
}

KeyboardBindings LoadKeyboardBindings() {
    auto bindings = DefaultKeyboardBindings;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (!values.HasKey(L"keyboard_bindings_v1")) return bindings;
        const auto stored = winrt::to_string(winrt::unbox_value<winrt::hstring>(values.Lookup(L"keyboard_bindings_v1")));
        const Common::ParamPackage package{stored};
        KeyboardBindings parsed{};
        for (size_t i = 0; i < parsed.size(); ++i) {
            const Common::ParamPackage key{package.Get(std::to_string(i), "")};
            const int code = key.Get("code", -1);
            if (key.Get("engine", "") != "keyboard" || code < 0 || code > 254 || code == 27)
                return bindings;
            if (!AssignKeyboardKey(parsed, i, static_cast<unsigned>(code))) return bindings;
        }
        return parsed;
    } catch (const winrt::hresult_error&) { return bindings; }
}
bool SaveKeyboardBindings(const KeyboardBindings& bindings) {
    try {
        Common::ParamPackage package;
        for (size_t i = 0; i < bindings.size(); ++i)
            package.Set(std::to_string(i), InputCommon::GenerateKeyboardParam(static_cast<int>(bindings[i])));
        winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values().Insert(
            L"keyboard_bindings_v1", winrt::box_value(winrt::to_hstring(package.Serialize())));
        return true;
    } catch (const winrt::hresult_error&) { return false; }
}
std::wstring KeyboardKeyName(unsigned key) {
    if (!key) return L"Sin asignar";
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= 112 && key <= 135) return L"F" + std::to_wstring(key - 111);
    if (key >= 96 && key <= 105) return L"Num " + std::to_wstring(key - 96);
    switch (key) {
    case 8: return L"Retroceso"; case 9: return L"Tab"; case 13: return L"Enter";
    case 16: return L"Shift"; case 17: return L"Ctrl"; case 18: return L"Alt";
    case 32: return L"Espacio"; case 33: return L"Re Pag"; case 34: return L"Av Pag";
    case 35: return L"Fin"; case 36: return L"Inicio"; case 37: return L"Izquierda";
    case 38: return L"Arriba"; case 39: return L"Derecha"; case 40: return L"Abajo";
    case 45: return L"Insert"; case 46: return L"Supr"; case 106: return L"Num *";
    case 107: return L"Num +"; case 109: return L"Num -"; case 110: return L"Num .";
    case 111: return L"Num /"; default: return L"Tecla " + std::to_wstring(key);
    }
}
void InitializeKeyboardBindings() {
    auto& runtime = KeyboardInput();
    std::scoped_lock lock{runtime.mutex};
    runtime.devices.clear();
    runtime.engine->ReleaseAllKeys();
    pressed_keys.store(0, std::memory_order_relaxed);
    const auto bindings = LoadKeyboardBindings();
    InputCommon::InputFactory factory{runtime.engine};
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (!bindings[i]) continue;
        auto device = factory.Create(Common::ParamPackage{InputCommon::GenerateKeyboardParam(static_cast<int>(bindings[i]))});
        device->SetCallback({[i](const Common::Input::CallbackStatus& status) {
            const u32 bit = 1U << i;
            if (status.button_status.value) pressed_keys.fetch_or(bit, std::memory_order_relaxed);
            else pressed_keys.fetch_and(~bit, std::memory_order_relaxed);
        }});
        runtime.devices.push_back(std::move(device));
    }
}
bool RunKeyboardInputSelfTest() {
    auto engine = std::make_shared<InputCommon::Keyboard>("keyboard");
    InputCommon::InputFactory factory{engine};
    bool down = false;
    unsigned edges = 0;
    const auto serialized = InputCommon::GenerateKeyboardParam('Q');
    const Common::ParamPackage params{serialized};
    if (params.Get("engine", "") != "keyboard" || params.Get("code", 0) != 'Q') return false;
    auto device = factory.Create(params);
    device->SetCallback({[&](const Common::Input::CallbackStatus& status) {
        down = status.button_status.value; ++edges;
    }});
    engine->PressKey('Q');
    if (!down || edges != 1) return false;
    engine->PressKey('Q');
    if (edges != 1) return false;
    engine->ReleaseKey('Q');
    if (down || edges != 2) return false;
    engine->PressKey('Q');
    engine->ReleaseAllKeys();
    if (down || edges != 4) return false;
    const u32 previous = pressed_keys.exchange((1U << 20) | (1U << 23) | (1U << 24));
    PadState state{};
    ApplyKeyboard(state);
    pressed_keys.store(previous);
    return state.left_x > 0.70f && state.left_x < 0.71f &&
           state.left_y == state.left_x && state.right_y == 1.0f;
}
void SetKeyboardKey(unsigned key, bool pressed) {
    auto& runtime = KeyboardInput();
    std::scoped_lock lock{runtime.mutex};
    if (pressed) runtime.engine->PressKey(static_cast<int>(key));
    else runtime.engine->ReleaseKey(static_cast<int>(key));
}
void ResetKeyboardKeys() {
    auto& runtime = KeyboardInput();
    std::scoped_lock lock{runtime.mutex};
    runtime.engine->ReleaseAllKeys();
    pressed_keys.store(0, std::memory_order_relaxed);
}

void RequestQuit() {
    quit_requested.store(true, std::memory_order_relaxed);
}

bool QuitRequested() {
    return quit_requested.load(std::memory_order_relaxed);
}

std::optional<InputStep> ParseInputStep(std::string_view spec) {
    InputStep step{.duration_ms = DEFAULT_STEP_MS, .text = std::string{spec}};
    const std::size_t first = spec.find(':');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string seconds{spec.substr(0, first)};
    char* end = nullptr;
    const double at = std::strtod(seconds.c_str(), &end);
    if (end == seconds.c_str() || at < 0.0) {
        return std::nullopt;
    }
    step.at_ms = static_cast<u32>(at * 1000.0);
    std::string_view buttons = spec.substr(first + 1);
    if (const std::size_t second = buttons.find(':'); second != std::string_view::npos) {
        const std::string duration{buttons.substr(second + 1)};
        step.duration_ms = static_cast<u32>(std::strtoul(duration.c_str(), nullptr, 10));
        buttons = buttons.substr(0, second);
    }
    while (!buttons.empty()) {
        const std::size_t plus = buttons.find('+');
        const std::string_view name = buttons.substr(0, plus);
        buttons = plus == std::string_view::npos ? std::string_view{} : buttons.substr(plus + 1);
        if (const auto it = std::ranges::find(BUTTON_NAMES, name, &ButtonName::name);
            it != BUTTON_NAMES.end()) {
            step.buttons |= Bit(it->button);
        } else if (name == "LS_UP") {
            step.left_y = 1.0f;
        } else if (name == "LS_DOWN") {
            step.left_y = -1.0f;
        } else if (name == "LS_LEFT") {
            step.left_x = -1.0f;
        } else if (name == "LS_RIGHT") {
            step.left_x = 1.0f;
        } else if (name == "RS_UP") {
            step.right_y = 1.0f;
        } else if (name == "RS_DOWN") {
            step.right_y = -1.0f;
        } else if (name == "RS_LEFT") {
            step.right_x = -1.0f;
        } else if (name == "RS_RIGHT") {
            step.right_x = 1.0f;
        } else {
            return std::nullopt;
        }
    }
    return step;
}

GamepadInput::GamepadInput(std::vector<InputStep> script_)
    : gamepad{std::make_shared<InputCommon::VirtualGamepad>(std::string{ENGINE})},
      script{std::move(script_)}, options{LoadControllerOptions()} {
    InitializeKeyboardBindings();
    Common::Input::RegisterInputFactory(std::string{ENGINE},
                                        std::make_shared<InputCommon::InputFactory>(gamepad));
    Common::Input::RegisterOutputFactory(std::string{ENGINE},
                                         std::make_shared<InputCommon::OutputFactory>(gamepad));
}

GamepadInput::~GamepadInput() {
    Stop();
    Common::Input::UnregisterInputFactory(std::string{ENGINE});
    Common::Input::UnregisterOutputFactory(std::string{ENGINE});
}

void GamepadInput::Start() {
    if (!thread.joinable()) {
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }
}

void GamepadInput::Stop() {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
    gamepad->ResetControllers();
    ResetKeyboardKeys();
}

void GamepadInput::Run(std::stop_token stop) {
    struct Apartment {
        Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        ~Apartment() { winrt::uninit_apartment(); }
    };
    std::optional<Apartment> apartment;
    bool controller_api_ready = true;
    try { apartment.emplace(); }
    catch (const winrt::hresult_error&) {
        LOG_ERROR(Input, "UWP input: cannot initialize controller apartment");
        controller_api_ready = false;
    }
    const auto start = std::chrono::steady_clock::now();
    std::optional<ControllerDevice> pad;
    PadState applied{};
    std::vector<bool> announced(script.size());
    u32 polls = 0;
    while (!stop.stop_requested()) {
        // The gamepad list is cheap to query but changes rarely: look again twice a second.
        if (controller_api_ready && polls++ % 125 == 0) {
            try {
                const auto devices = EnumerateControllers();
                const bool had_pad = pad.has_value();
                const auto selected = SelectController(devices, options.controller_id);
                pad = selected ? std::optional<ControllerDevice>{devices[*selected]} : std::nullopt;
                if (pad.has_value() != had_pad) {
                    LOG_INFO(Input, "UWP input: controller {} ({} connected)",
                             pad ? "in use as player 1" : "disconnected", devices.size());
                }
            } catch (...) {
                pad.reset();
            }
        }
        PadState state{};
        if (pad) {
            try {
                state = ReadGamepad(*pad, options);
            } catch (...) {
                pad.reset();
            }
        }
        ApplyKeyboard(state);
        const auto elapsed = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - start)
                                                  .count());
        for (std::size_t index = 0; index < script.size(); ++index) {
            const InputStep& step = script[index];
            if (elapsed < step.at_ms || elapsed >= step.at_ms + step.duration_ms) {
                continue;
            }
            if (!announced[index]) {
                announced[index] = true;
                LOG_INFO(Input, "UWP input: script step {} at {} ms", step.text, elapsed);
            }
            state.buttons |= step.buttons;
            // A scripted stick direction wins over the pad's.
            if (step.left_x != 0.0f || step.left_y != 0.0f) {
                state.left_x = step.left_x;
                state.left_y = step.left_y;
            }
            if (step.right_x != 0.0f || step.right_y != 0.0f) {
                state.right_x = step.right_x;
                state.right_y = step.right_y;
            }
        }
        if (state != applied) {
            for (u32 button = 0; button < NUM_BUTTONS; ++button) {
                const u32 bit = 1U << button;
                if ((state.buttons ^ applied.buttons) & bit) {
                    gamepad->SetButtonState(PLAYER, static_cast<int>(button),
                                            (state.buttons & bit) != 0);
                }
            }
            if (state.left_x != applied.left_x || state.left_y != applied.left_y) {
                gamepad->SetStickPosition(PLAYER, VirtualStick::Left, state.left_x, state.left_y);
            }
            if (state.right_x != applied.right_x || state.right_y != applied.right_y) {
                gamepad->SetStickPosition(PLAYER, VirtualStick::Right, state.right_x,
                                          state.right_y);
            }
            applied = state;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
}

} // namespace EdenXbox
