// SPDX-FileCopyrightText: 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>

#include "core/core.h"
#include "core/core_timing.h"

namespace {
// Numbers are chosen randomly to make sure the correct one is given.
constexpr std::array<u64, 5> calls_order{{2, 0, 1, 4, 3}};
std::array<s64, 5> delays{};
std::bitset<5> callbacks_ran_flags;
u64 expected_callback = 0;

template <unsigned int IDX>
std::optional<std::chrono::nanoseconds> HostCallbackTemplate(s64 time,
                                                             std::chrono::nanoseconds ns_late) {
    static_assert(IDX < callbacks_ran_flags.size(), "IDX out of range");
    callbacks_ran_flags.set(IDX);
    delays[IDX] = ns_late.count();
    ++expected_callback;
    return std::nullopt;
}

struct ScopeInit final {
    ScopeInit() {
        core_timing.SetMulticore(true);
        core_timing.Initialize([]() {});
    }

    Core::Timing::CoreTiming core_timing;
};

u64 TestTimerSpeed(Core::Timing::CoreTiming& core_timing) {
    const u64 start = core_timing.GetGlobalTimeNs().count();
    volatile u64 placebo = 0;
    for (std::size_t i = 0; i < 1000; i++) {
        placebo = placebo + core_timing.GetGlobalTimeNs().count();
    }
    const u64 end = core_timing.GetGlobalTimeNs().count();
    return end - start;
}

} // Anonymous namespace

TEST_CASE("CoreTiming[Pulse reconstructs owners and FIFO]", "[core][pulse]") {
    using namespace Core::Timing;
    std::vector<int> calls;
    TimingCheckpoint saved{};
    {
        CoreTiming source;
        source.SetMulticore(false);
        source.Initialize([] {});
        auto first = CreateEvent("same-name", [&](s64, auto) {
            calls.push_back(-1);
            return std::optional<std::chrono::nanoseconds>{};
        });
        auto second = CreateEvent("same-name", [&](s64, auto) {
            calls.push_back(-2);
            return std::optional<std::chrono::nanoseconds>{};
        });
        const std::array bindings{CheckpointEventBinding{1, first},
                                  CheckpointEventBinding{2, second}};
        source.ScheduleEvent(std::chrono::nanoseconds{0}, second);
        source.ScheduleEvent(std::chrono::nanoseconds{0}, first);
        saved = source.CaptureCheckpoint(bindings);
        REQUIRE(calls.empty());
        REQUIRE_THROWS_AS(source.CaptureCheckpoint({}), std::invalid_argument);
    }
    // Every original owner and callback has been destroyed. IDs bind fresh callbacks.
    CoreTiming restored;
    restored.SetMulticore(false);
    restored.Initialize([] {});
    auto first = CreateEvent("same-name", [&](s64, auto) {
        calls.push_back(1);
        return std::optional<std::chrono::nanoseconds>{};
    });
    auto second = CreateEvent("same-name", [&](s64, auto) {
        calls.push_back(2);
        return std::optional<std::chrono::nanoseconds>{};
    });
    const std::array bindings{CheckpointEventBinding{1, first},
                              CheckpointEventBinding{2, second}};
    restored.RestoreCheckpoint(saved, bindings);
    auto invalid = saved;
    invalid.events.back().fifo_order = invalid.events.front().fifo_order;
    REQUIRE_THROWS_AS(restored.RestoreCheckpoint(invalid, bindings), std::invalid_argument);
    invalid = saved;
    invalid.events.front().binding_id = 99;
    REQUIRE_THROWS_AS(restored.RestoreCheckpoint(invalid, bindings), std::invalid_argument);
    REQUIRE(calls.empty());
    restored.Advance();
    REQUIRE(calls == std::vector<int>{2, 1});
}

TEST_CASE("CoreTiming[BasicOrder]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;
    std::vector<std::shared_ptr<Core::Timing::EventType>> events{
        Core::Timing::CreateEvent("callbackA", HostCallbackTemplate<0>),
        Core::Timing::CreateEvent("callbackB", HostCallbackTemplate<1>),
        Core::Timing::CreateEvent("callbackC", HostCallbackTemplate<2>),
        Core::Timing::CreateEvent("callbackD", HostCallbackTemplate<3>),
        Core::Timing::CreateEvent("callbackE", HostCallbackTemplate<4>),
    };

    expected_callback = 0;

    core_timing.SyncPause(true);

    const u64 one_micro = 1000U;
    for (std::size_t i = 0; i < events.size(); i++) {
        const u64 order = calls_order[i];
        const auto future_ns = std::chrono::nanoseconds{static_cast<s64>(i * one_micro + 100)};

        core_timing.ScheduleEvent(future_ns, events[order]);
    }
    /// test pause
    REQUIRE(callbacks_ran_flags.none());

    core_timing.Pause(false); // No need to sync

    while (core_timing.HasPendingEvents())
        ;

    REQUIRE(callbacks_ran_flags.all());

    for (std::size_t i = 0; i < delays.size(); i++) {
        const double delay = static_cast<double>(delays[i]);
        const double micro = delay / 1000.0f;
        const double mili = micro / 1000.0f;
        printf("HostTimer Pausing Delay[%zu]: %.3f %.6f\n", i, micro, mili);
    }
}

TEST_CASE("CoreTiming[BasicOrderNoPausing]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;
    std::vector<std::shared_ptr<Core::Timing::EventType>> events{
        Core::Timing::CreateEvent("callbackA", HostCallbackTemplate<0>),
        Core::Timing::CreateEvent("callbackB", HostCallbackTemplate<1>),
        Core::Timing::CreateEvent("callbackC", HostCallbackTemplate<2>),
        Core::Timing::CreateEvent("callbackD", HostCallbackTemplate<3>),
        Core::Timing::CreateEvent("callbackE", HostCallbackTemplate<4>),
    };

    core_timing.SyncPause(true);
    core_timing.SyncPause(false);

    expected_callback = 0;

    const u64 start = core_timing.GetGlobalTimeNs().count();
    const u64 one_micro = 1000U;

    for (std::size_t i = 0; i < events.size(); i++) {
        const u64 order = calls_order[i];
        const auto future_ns = std::chrono::nanoseconds{static_cast<s64>(i * one_micro + 100)};
        core_timing.ScheduleEvent(future_ns, events[order]);
    }

    const u64 end = core_timing.GetGlobalTimeNs().count();
    const double scheduling_time = static_cast<double>(end - start);
    const double timer_time = static_cast<double>(TestTimerSpeed(core_timing));

    while (core_timing.HasPendingEvents())
        ;

    REQUIRE(callbacks_ran_flags.all());

    for (std::size_t i = 0; i < delays.size(); i++) {
        const double delay = static_cast<double>(delays[i]);
        const double micro = delay / 1000.0f;
        const double mili = micro / 1000.0f;
        printf("HostTimer No Pausing Delay[%zu]: %.3f %.6f\n", i, micro, mili);
    }

    const double micro = scheduling_time / 1000.0f;
    const double mili = micro / 1000.0f;
    printf("HostTimer No Pausing Scheduling Time: %.3f %.6f\n", micro, mili);
    printf("HostTimer No Pausing Timer Time: %.3f %.6f\n", timer_time / 1000.f,
           timer_time / 1000000.f);
}
