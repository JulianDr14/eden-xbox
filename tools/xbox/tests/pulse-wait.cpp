// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <iostream>
#include <random>
#include "core/hle/kernel/pulse_wait_state.h"

namespace {
void Require(bool value) { if (!value) throw std::runtime_error("Pulse wait assertion"); }
template <class F> void Reject(F operation) {
    bool rejected{};
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected);
}
}

int main() {
    try {
        std::mt19937_64 random{0x57414954};
        for (size_t cycle = 0; cycle < 100; ++cycle) {
            Kernel::ThreadWaitCheckpoint sleep{Kernel::CheckpointWaitKind::Sleep,
                                                static_cast<s64>(random() >> 1) | 1, {}};
            const auto encoded_sleep = Kernel::EncodeWaitCheckpoint(sleep);
            const auto restored_sleep = Kernel::DecodeWaitCheckpoint(encoded_sleep);
            Require(restored_sleep.kind == sleep.kind && restored_sleep.deadline == sleep.deadline);
            std::array<u64, 8> arguments{};
            for (auto& value : arguments) value = random();
            const auto original = arguments;
            Kernel::CompleteWaitArguments(restored_sleep.kind, 0xbeef, -1, arguments);
            Require(arguments == original);

            Kernel::ThreadWaitCheckpoint sync{Kernel::CheckpointWaitKind::Synchronization, 0, {}};
            const auto count = cycle % 65;
            for (size_t i = 0; i < count; ++i) sync.objects.push_back({(i % 11) + 1, i / 11});
            const auto encoded = Kernel::EncodeWaitCheckpoint(sync);
            const auto restored = Kernel::DecodeWaitCheckpoint(encoded);
            Require(restored.objects == sync.objects && restored.deadline == 0 && restored.kind == sync.kind);
            Kernel::CompleteWaitArguments(restored.kind, 0xdeadbeef, -1, arguments);
            Require(arguments[0] == 0xdeadbeef && arguments[1] == 0xffffffff);
            for (size_t i = 2; i < 8; ++i) Require(arguments[i] == original[i]);
            for (size_t truncated = 0; truncated < encoded.size(); ++truncated) {
                Reject([&] { Kernel::DecodeWaitCheckpoint(std::span{encoded}.first(truncated)); });
            }
            auto invalid = encoded;
            invalid[4] = 65;
            Reject([&] { Kernel::DecodeWaitCheckpoint(invalid); });
            invalid = encoded; invalid[0] = 0xff;
            Reject([&] { Kernel::DecodeWaitCheckpoint(invalid); });
            invalid = encoded; invalid[15] |= 0x80;
            Reject([&] { Kernel::DecodeWaitCheckpoint(invalid); });

            Kernel::ThreadWaitCheckpoint address{Kernel::CheckpointWaitKind::AddressArbiter,
                sleep.deadline, {}, 0x8000000 + cycle * 4, cycle};
            const auto restored_address = Kernel::DecodeWaitCheckpoint(Kernel::EncodeWaitCheckpoint(address));
            Require(restored_address.address == address.address &&
                    restored_address.ordinal == address.ordinal &&
                    restored_address.deadline == address.deadline);
            arguments = original;
            Kernel::CompleteWaitArguments(restored_address.kind, 0xcafebabe, 0, arguments);
            Require(arguments[0] == 0xcafebabe);
            for (size_t i = 1; i < 8; ++i) Require(arguments[i] == original[i]);
        }
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::Sleep, 0, {}}); });
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::Synchronization, 0, {{0, 0}}}); });
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::None, 0, {}}); });
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::AddressArbiter, 0, {}, 3, 0}); });
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::AddressArbiter, 0, {}, 4096, 65536}); });
        std::cout << "Pulse wait: 100 wire/ABI cycles, duplicate IDs, truncation and limits passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
