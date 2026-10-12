// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <iostream>
#include <random>
#include "core/hle/kernel/pulse_wait_state.h"
#include "core/hle/kernel/pulse_wait_graph.h"

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
            sleep.timer_ordinal = cycle;
            const auto encoded_sleep = Kernel::EncodeWaitCheckpoint(sleep);
            const auto restored_sleep = Kernel::DecodeWaitCheckpoint(encoded_sleep);
            Require(restored_sleep.kind == sleep.kind && restored_sleep.deadline == sleep.deadline &&
                    restored_sleep.timer_ordinal == sleep.timer_ordinal);
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
            Kernel::ThreadWaitCheckpoint mutex{Kernel::CheckpointWaitKind::Mutex, 0,
                {{101, cycle}}, 0x8001000 + cycle * 4, 0, 0, 202, static_cast<u32>(random())};
            auto restored_mutex = Kernel::DecodeWaitCheckpoint(Kernel::EncodeWaitCheckpoint(mutex));
            Require(restored_mutex.objects == mutex.objects && restored_mutex.reference_owner == 202 &&
                    restored_mutex.tag == mutex.tag && restored_mutex.address == mutex.address);
            auto condition = mutex;
            condition.kind = Kernel::CheckpointWaitKind::ConditionVariable;
            condition.reference_owner = 0;
            condition.condition_key = 0x8003000;
            condition.deadline = sleep.deadline;
            for (bool in_tree : {false, true}) {
                condition.objects = in_tree ? std::vector<Kernel::WaitObjectReference>{} : mutex.objects;
                condition.ordinal = in_tree ? cycle : 0;
                auto rebuilt = Kernel::DecodeWaitCheckpoint(Kernel::EncodeWaitCheckpoint(condition));
                Require(rebuilt.objects == condition.objects && rebuilt.ordinal == condition.ordinal &&
                        rebuilt.condition_key == condition.condition_key && rebuilt.tag == condition.tag);
                arguments = original;
                Kernel::CompleteWaitArguments(rebuilt.kind, 0x11223344, -1, arguments);
                Require(arguments[0] == 0x11223344);
                for (size_t i = 1; i < 8; ++i) Require(arguments[i] == original[i]);
            }
            arguments = original;
            Kernel::CompleteWaitArguments(restored_mutex.kind, 0x55667788, -1, arguments);
            Require(arguments[0] == 0x55667788);
            for (size_t i = 1; i < 8; ++i) Require(arguments[i] == original[i]);
            auto bad_mutex = mutex; bad_mutex.reference_owner = 0;
            Reject([&] { Kernel::EncodeWaitCheckpoint(bad_mutex); });
            bad_mutex = mutex; bad_mutex.deadline = 1;
            Reject([&] { Kernel::EncodeWaitCheckpoint(bad_mutex); });
            auto bad_condition = condition; bad_condition.condition_key = 3;
            Reject([&] { Kernel::EncodeWaitCheckpoint(bad_condition); });
            bad_condition = condition; bad_condition.objects = mutex.objects; bad_condition.ordinal = 1;
            Reject([&] { Kernel::EncodeWaitCheckpoint(bad_condition); });
            auto bad_wire = Kernel::EncodeWaitCheckpoint(mutex); bad_wire[52] = 1;
            Reject([&] { Kernel::DecodeWaitCheckpoint(bad_wire); });
        }
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::Sleep, 0, {}}); });
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::Synchronization, 0, {{0, 0}}}); });
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::None, 0, {}}); });
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::AddressArbiter, 0, {}, 3, 0}); });
        Reject([] { Kernel::EncodeWaitCheckpoint({Kernel::CheckpointWaitKind::AddressArbiter, 0, {}, 4096, 65536}); });
        for (size_t cycle = 0; cycle < 100; ++cycle) {
            std::vector<Kernel::CheckpointWaitingThread> threads;
            // Two object FIFOs and a common timeout must impose the same order,
            // even when the thread IDs and input descriptors are shuffled.
            for (size_t i = 0; i < 64; ++i) {
                Kernel::ThreadWaitCheckpoint wait{Kernel::CheckpointWaitKind::Synchronization,
                    123456, {{10, i}, {20, i}}, 0, 0, 0, 0, 0, i};
                threads.push_back({1000 - i, 1, 16, wait});
            }
            std::shuffle(threads.begin(), threads.end(), random);
            const auto order = Kernel::PlanCheckpointWaitRestore(threads);
            Require(order.size() == 64);
            for (size_t i = 0; i < order.size(); ++i) Require(threads[order[i]].wait.timer_ordinal == i);
            auto bad = threads;
            bad[0].wait.timer_ordinal = bad[1].wait.timer_ordinal;
            Reject([&] { Kernel::PlanCheckpointWaitRestore(bad); });
            bad = threads; bad[0].id = bad[1].id;
            Reject([&] { Kernel::PlanCheckpointWaitRestore(bad); });
            bad = threads; bad[0].wait.objects[0].ordinal = 65535;
            Reject([&] { Kernel::PlanCheckpointWaitRestore(bad); });
            // Individually valid object lists with inconsistent cross-list order.
            std::vector<Kernel::CheckpointWaitingThread> cyclic{
                {1, 1, 16, {Kernel::CheckpointWaitKind::Synchronization, 0, {{10, 0}, {20, 1}}}},
                {2, 1, 16, {Kernel::CheckpointWaitKind::Synchronization, 0, {{10, 1}, {20, 0}}}}};
            Reject([&] { Kernel::PlanCheckpointWaitRestore(cyclic); });
            // Duplicate handles in one wait are separate list nodes, not threads.
            cyclic[0].wait.objects = {{10, 0}, {10, 1}};
            cyclic[1].wait.objects = {{10, 2}};
            const auto duplicates = Kernel::PlanCheckpointWaitRestore(cyclic);
            Require(duplicates.size() == 2 && duplicates[0] == 0 && duplicates[1] == 1);
        }
        Reject([] {
            std::vector<Kernel::CheckpointWaitingThread> oversized(4097);
            Kernel::PlanCheckpointWaitRestore(oversized);
        });
        std::cout << "Pulse wait: 100 wire/ABI cycles, mutex/condition phases, distinct retained owners, timer FIFO, shuffled graph ordering and malformed inputs passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
