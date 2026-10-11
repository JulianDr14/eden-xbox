// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <thread>
#include "core/core_timing.h"

int main() {
    using namespace Core::Timing;
    try {
        for (int cycle = 0; cycle < 100; ++cycle) {
            std::vector<int> calls;
            TimingCheckpoint saved{};
            {
                CoreTiming source;
                source.SetMulticore(false);
                source.Initialize([] {});
                auto callback = [](s64, auto) {
                    throw std::runtime_error("original callback executed");
                    return std::optional<std::chrono::nanoseconds>{};
                };
                auto a = CreateEvent("same", callback);
                auto b = CreateEvent("same", callback);
                const std::array bindings{CheckpointEventBinding{1, a},
                                          CheckpointEventBinding{2, b}};
                source.ScheduleEvent(std::chrono::nanoseconds{0}, b);
                source.ScheduleEvent(std::chrono::nanoseconds{0}, a);
                saved = source.CaptureCheckpoint(bindings);
                bool rejected = false;
                try { (void)source.CaptureCheckpoint({}); }
                catch (const std::invalid_argument&) { rejected = true; }
                if (!rejected) throw std::runtime_error("missing owner accepted");
            }
            CoreTiming target;
            target.SetMulticore(false);
            target.Initialize([] {});
            auto a = CreateEvent("same", [&](s64, auto) {
                calls.push_back(1);
                return std::optional<std::chrono::nanoseconds>{};
            });
            auto b = CreateEvent("same", [&](s64, auto) {
                calls.push_back(2);
                return std::optional<std::chrono::nanoseconds>{};
            });
            const std::array bindings{CheckpointEventBinding{1, a},
                                      CheckpointEventBinding{2, b}};
            target.RestoreCheckpoint(saved, bindings);
            auto invalid = saved;
            invalid.events[1].fifo_order = invalid.events[0].fifo_order;
            bool rejected = false;
            try { target.RestoreCheckpoint(invalid, bindings); }
            catch (const std::invalid_argument&) { rejected = true; }
            if (!rejected) throw std::runtime_error("duplicate FIFO accepted");
            target.Advance();
            if (calls != std::vector<int>{2, 1}) throw std::runtime_error("FIFO changed");
        }
        {
            CoreTiming source;
            source.SetMulticore(true);
            source.Initialize([] {});
            source.SyncPause(true);
            const auto saved = source.CaptureCheckpoint({});
            CoreTiming target;
            target.SetMulticore(true);
            target.Initialize([] {});
            target.SyncPause(true);
            target.RestoreCheckpoint(saved, {});
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            target.Pause(false);
            const auto elapsed = target.GetGlobalTimeNs().count() - saved.time_ns;
            if (elapsed < 0 || elapsed > 5'000'000) {
                throw std::runtime_error("reconstruction time leaked into guest clock");
            }
            target.SyncPause(true);
        }
        {
            CoreTiming timing;
            timing.SetMulticore(false);
            timing.Initialize([] {});
            int count = 0;
            auto event = CreateEvent("loop", [&](s64, auto) {
                ++count;
                return std::optional<std::chrono::nanoseconds>{};
            });
            const std::array bindings{CheckpointEventBinding{1, event}};
            timing.ScheduleLoopingEvent(std::chrono::nanoseconds{0},
                                        std::chrono::nanoseconds{1000}, event);
            const auto state = timing.CaptureCheckpoint(bindings);
            timing.RestoreCheckpoint(state, bindings);
            timing.Advance();
            if (count != 1) throw std::runtime_error("restored loop failed");
            timing.UnscheduleEvent(event);
            if (!timing.CaptureCheckpoint(bindings).events.empty()) {
                throw std::runtime_error("restored loop handle invalid");
            }
        }
        std::cout << "Pulse timing: 100 owner reconstruction/FIFO/invalid-state cycles passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
