// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include "dynarmic/interface/exclusive_monitor.h"
#include "core/pulse_machine_state.h"

namespace {
void Require(bool value) { if (!value) throw std::runtime_error("Pulse exclusive assertion"); }
template <class F> void Reject(F operation) {
    bool rejected{};
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected);
}
}

int main() {
    try {
        std::mt19937_64 random{0x4558434c};
        for (size_t cycle = 0; cycle < 100; ++cycle) {
            auto original = std::make_unique<Dynarmic::ExclusiveMonitor>(4);
            const auto value = random();
            original->ReadAndMark<uint64_t>(0, 0x1000, [&] { return value; });
            original->ReadAndMark<uint64_t>(1, 0x1000, [&] { return value; });
            const Dynarmic::Vector wide{random(), random()};
            original->ReadAndMark<Dynarmic::Vector>(2, 0x2000, [&] { return wide; });
            original->ReadAndMark<uint8_t>(3, 0x3000, [] { return uint8_t{42}; });
            original->ClearProcessor(3);
            const auto captured = original->CaptureCheckpoint();
            Core::ExclusiveMonitorCheckpoint logical{};
            logical.core_count = static_cast<u32>(captured.processor_count);
            for (size_t i = 0; i < logical.reservations.size(); ++i) {
                logical.reservations[i] = {captured.reservations[i].address,
                                          captured.reservations[i].value, captured.reservations[i].active};
            }
            const auto wire = Core::Pulse::EncodeExclusiveState(logical);
            original.reset();
            const auto decoded = Core::Pulse::DecodeExclusiveState(wire);
            Dynarmic::ExclusiveMonitor::Checkpoint state{};
            state.processor_count = decoded.core_count;
            for (size_t i = 0; i < state.reservations.size(); ++i) {
                state.reservations[i] = {decoded.reservations[i].address,
                                        decoded.reservations[i].value, decoded.reservations[i].active};
            }
            for (size_t truncate = 0; truncate < wire.size(); ++truncate) {
                bool rejected{};
                try { Core::Pulse::DecodeExclusiveState(std::span{wire}.first(truncate)); }
                catch (const Common::Pulse::Failure&) { rejected = true; }
                Require(rejected);
            }
            for (size_t bad_offset : {size_t{0}, size_t{4}, size_t{32}, size_t{36}, size_t{104}}) {
                auto malformed = wire;
                malformed[bad_offset] = 0xff;
                bool rejected{};
                try { Core::Pulse::DecodeExclusiveState(malformed); }
                catch (const Common::Pulse::Failure&) { rejected = true; }
                Require(rejected);
            }
            Dynarmic::ExclusiveMonitor restored(4);
            restored.RestoreCheckpoint(state);
            Require(restored.DoExclusiveOperation<uint64_t>(0, 0x1000, [&](auto expected) { return expected == value; }));
            // Successful store invalidates the peer's reservation too.
            Require(!restored.DoExclusiveOperation<uint64_t>(1, 0x1000, [](auto) { return true; }));
            Require(!restored.DoExclusiveOperation<uint8_t>(3, 0x3000, [](auto) { return true; }));
            // Wrong address must not consume a different reservation.
            Require(!restored.DoExclusiveOperation<Dynarmic::Vector>(2, 0x3000, [](auto) { return true; }));
            Require(restored.DoExclusiveOperation<Dynarmic::Vector>(2, 0x2000, [&](auto expected) { return expected == wide; }));
            Require(!restored.DoExclusiveOperation<Dynarmic::Vector>(2, 0x2000, [](auto) { return true; }));
            restored.RestoreCheckpoint(state);
            auto bad = state; bad.processor_count = 3;
            Reject([&] { restored.RestoreCheckpoint(bad); });
            bad = state; bad.reservations[3].value[0] = 1;
            Reject([&] { restored.RestoreCheckpoint(bad); });
            bad = state; bad.reservations[0].address = 0xdeaddeaddeaddeadULL;
            Reject([&] { restored.RestoreCheckpoint(bad); });
            // Malformed inputs are rejected before touching valid reservations.
            Require(restored.DoExclusiveOperation<uint64_t>(0, 0x1000, [&](auto expected) { return expected == value; }));
            restored.Clear();
            const auto empty = restored.CaptureCheckpoint();
            for (const auto& reservation : empty.reservations) Require(!reservation.active && !reservation.address && reservation.value == Dynarmic::Vector{});
        }
        std::cout << "Pulse exclusive: 100 real Dynarmic destruction/restore cycles, alias invalidation and malformed state passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
