// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <stdexcept>
#include "eden_uwp/controller_catalog.h"
#include "eden_uwp/menu_input_capture.h"
#include "eden_uwp/list_navigation.h"
#include "eden_uwp/text_entry_input.h"

struct Device {
    std::wstring id;
    bool ready{true};
    explicit operator bool() const { return ready; }
    bool operator==(const Device&) const = default;
};
struct State {
    std::uint32_t buttons{};
    float left_x{}, left_y{}, right_x{}, right_y{};
};
int main() {
    using namespace EdenXbox;
    using namespace std::chrono_literals;
    ControllerCatalog<Device> catalog;
    auto now = ControllerCatalog<Device>::Clock::time_point{};
    std::vector<Device> hardware{{L"unsupported", false}, {L"one"}, {L"two"}};
    int calls = 0;
    auto discover = [&] { ++calls; return hardware; };
    auto equal = [](const auto& a, const auto& b) { return a == b; };
    assert(catalog.Update(now, discover, equal));
    assert(catalog.Select(L"")->id == L"one");
    assert(!catalog.Select(KeyboardControllerId));
    assert(catalog.Select(L"two")->id == L"two");
    assert(!catalog.Update(now + 499ms, discover, equal) && calls == 1);
    assert(!catalog.Update(now + 500ms, discover, equal) && calls == 2);
    hardware.erase(hardware.begin() + 1);
    assert(catalog.Update(now + 1s, discover, equal));
    assert(!catalog.Select(L"one")); // Never redirect explicit selection on unplug.
    assert(catalog.Select(L"")->id == L"two");
    hardware.insert(hardware.begin(), Device{L"one"});
    assert(catalog.Update(now + 2s, discover, equal));
    assert(catalog.Select(L"one")->id == L"one");
    assert(calls == 4); // One query after a stall, no catch-up loop.
    try {
        catalog.Update(now + 3s, []() -> std::vector<Device> { throw std::runtime_error("offline"); }, equal);
        assert(false);
    } catch (const std::runtime_error&) {}
    assert(catalog.Select(L"one")); // No partial publication after discovery failure.
    assert(!catalog.Update(now + 3100ms, discover, equal));
    catalog.ReadFailed();
    assert(!catalog.Select(L"one"));
    assert(!catalog.Update(now + 3200ms, discover, equal));
    assert(!catalog.Select(L"one"));
    assert(!catalog.Update(now + 3500ms, discover, equal));
    assert(catalog.Select(L"one")); // Retry only after the scheduled successful query.
    ControllerCatalog<Device> next_session;
    assert(next_session.Update(now, discover, equal)); // Lifetime starts with a fresh query.

    // Exhaustively verify viewport invariants across growth/shrink and stale selection.
    for (std::size_t count = 0; count < 40; ++count)
        for (std::size_t visible = 1; visible < 12; ++visible)
            for (std::size_t cursor = 0; cursor < 50; ++cursor)
                for (std::size_t start = 0; start < 50; ++start) {
                    auto selected = cursor, first = start;
                    ClampListWindow(selected, first, count, visible);
                    assert(selected == (count ? std::min(cursor, count - 1) : 0));
                    assert(first <= selected && selected - first < visible);
                    assert(first <= (count > visible ? count - visible : 0));
                }
    using Command = TextEntry::Command;
    assert(!TextEntryCommand(MenuAction::Toggle));
    assert(TextEntryCommand(MenuAction::Confirm) == Command::Press);
    assert(TextEntryCommand(MenuAction::Back) == Command::Cancel);
    assert(TextEntryCommand(MenuAction::Alternate) == Command::Delete);
    assert(TextEntryCommand(MenuAction::Refresh) == Command::Space);
    assert(TextEntryCommand(MenuAction::PreviousTab) == Command::Shift);
    assert(TextEntryCommand(MenuAction::NextTab) == Command::Done);
    MenuInputCapture capture;
    State state{3, 1, 0, 0, -1};
    capture.Filter(state, true);
    assert(!state.buttons && !state.left_x && !state.right_y);
    state = {7, 1, 0, 0, -1};
    capture.Filter(state, false);
    assert(state.buttons == 4 && !state.left_x && !state.right_y); // New button passes.
    state = {2};
    capture.Filter(state, false);
    assert(!state.buttons);
    state = {1, 0.5f, 0};
    capture.Filter(state, false);
    assert(state.buttons == 1 && state.left_x == 0.5f); // Released then pressed again.
    state = {};
    capture.Filter(state, true);
    state = {8};
    capture.Filter(state, false);
    assert(state.buttons == 8); // Reopening cannot retain stale consumed buttons.
}
