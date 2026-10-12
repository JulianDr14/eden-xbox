// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Malformed-input gate for the process coordinator's sections (core/pulse_process_state.h): every
// truncation, every single-byte corruption and every hand-made inconsistency must either decode
// to a valid state or fail with Common::Pulse::Failure, before a fresh System is touched.
#include <functional>
#include <iostream>
#include <random>
#include "core/pulse_process_state.h"

namespace {
using namespace Core::Pulse;

void Require(bool value, const char* what) {
    if (!value) throw std::runtime_error(std::string{"Pulse process assertion: "} + what);
}

template <class F>
void Rejects(F operation, const char* what) {
    bool rejected{};
    try {
        operation();
    } catch (const Common::Pulse::Failure& failure) {
        rejected = failure.code == Common::Pulse::Error::Invalid;
    }
    Require(rejected, what);
}

// Truncations and one-byte corruptions: never a crash, never another exception type; a
// corruption that still decodes must re-encode to exactly the bytes that were decoded.
template <class Decode, class Encode>
size_t Mutate(const std::vector<u8>& bytes, Decode decode, Encode encode, std::mt19937_64& random) {
    size_t accepted{};
    for (size_t size = 0; size < bytes.size(); ++size) {
        Rejects([&] { (void)decode(std::span{bytes}.first(size)); }, "truncated section accepted");
    }
    auto extended = bytes;
    extended.push_back(0);
    Rejects([&] { (void)decode(extended); }, "trailing byte accepted");
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto corrupt = bytes;
        corrupt[i] ^= static_cast<u8>(1u << (random() % 8));
        try {
            const auto state = decode(corrupt);
            Require(encode(state) == corrupt, "corruption decoded to a different encoding");
            ++accepted;
        } catch (const Common::Pulse::Failure& failure) {
            Require(failure.code == Common::Pulse::Error::Invalid, "corruption failed with a wrong code");
        }
    }
    return accepted;
}

ProcessLayout MakeLayout() {
    ProcessLayout s;
    s.program_id = 0x0100000000001000;
    s.process_id = 0x51;
    s.pin = {0x8000000, {0x1000000, 0x2000000, 0, 0x4000000}, 0x6000000000, {1, 2, 3, 4}};
    s.main_stack_address = 0x7000000000;
    s.main_stack_size = 0x100000;
    s.heap_start = 0x8000000000;
    s.heap_size = 0x2000000;
    s.blocks = {{0x8000000, 0x10000, 0x00DC7E03, 5, 0},
                {0x8010000, 0x4000, 0x03FEBD04, 3, 0},
                {0x8000000000, 0x30000, 0x077EBD05, 3, 0},
                {0x8000030000, 0x10000, 0x077EBD05, 0, 2}};
    s.aliases = {{0x7100000000, 0x8000030000, 0x10000}};
    s.runs = {{0x8000000, 0x14}, {0x7000000000, 0x100}, {0x8000000000, 0x30}};
    return s;
}

std::vector<ObjectRecord> MakeObjects() {
    return {{ProcessObjectId, ObjectKind::Process, 0, 0, {}},
            {5, ObjectKind::Event, 0, 0, {}},
            {6, ObjectKind::ReadableEvent, 1, 5, {}},
            {7, ObjectKind::ClientSession, 0, 0, {1, 0, 0, 0, 4, 0, 0, 0}}};
}

std::vector<ThreadRecord> MakeThreads(std::mt19937_64& random) {
    std::vector<ThreadRecord> threads;
    for (u64 i = 0; i < 5; ++i) {
        ThreadRecord t{};
        t.id = 2 + i;
        t.thread_id = 0x48 + i;
        t.tls = 0x6000000200 + i * 0x200;
        t.run = static_cast<ThreadRun>(i % 3);
        t.flags = i == 4;
        t.base_priority = 0x2c;
        t.ideal_core = static_cast<s32>(i % 3);
        t.affinity = 0x7;
        for (auto& byte : t.context) byte = static_cast<u8>(random());
        threads.push_back(t);
    }
    return threads;
}

std::vector<Kernel::CheckpointWaitingThread> MakeWaits() {
    using Kernel::CheckpointWaitKind;
    std::vector<Kernel::CheckpointWaitingThread> waits;
    // Same deadline: both share one timer bucket, ordered by timer_ordinal.
    Kernel::ThreadWaitCheckpoint sleep{CheckpointWaitKind::Sleep, 3000000000, {}};
    waits.push_back({2, ProcessObjectId, 0x2c, sleep});
    Kernel::ThreadWaitCheckpoint event{CheckpointWaitKind::Synchronization, 3000000000, {{6, 0}}};
    event.timer_ordinal = 0;
    waits.push_back({3, ProcessObjectId, 0x2c, event});
    waits[0].wait.timer_ordinal = 1;
    Kernel::ThreadWaitCheckpoint condition{CheckpointWaitKind::ConditionVariable, 0, {}};
    condition.address = 0x8000001000;
    condition.condition_key = 0x8000001010;
    waits.push_back({4, ProcessObjectId, 0x2c, condition});
    return waits;
}

TimingSection MakeTiming() {
    TimingSection s;
    s.multicore = true;
    s.time_ns = 5000000000;
    s.clock_ticks = 96000000;
    s.gpu_ticks = 3840000000;
    s.cpu_ticks = 1;
    s.downcount = 2;
    s.pause_end_time = 4000000000;
    s.next_fifo_id = 40;
    s.bindings = {{1, "ScreenComposition"}, {3, "HID::UpdatePadCallback"}};
    s.events = {{1, 5016666666, 39, 16666666}, {3, 5004000000, 12, 0}};
    return s;
}
} // namespace

int main() {
    try {
        std::mt19937_64 random{0x50524f43};
        size_t accepted{};

        const auto layout = MakeLayout();
        const auto layout_bytes = EncodeProcessLayout(layout);
        Require(DecodeProcessLayout(layout_bytes) == layout, "layout round trip");
        Require(layout.ContentSize() == (0x14 + 0x100 + 0x30) * GuestPageSize, "content size");
        accepted += Mutate(layout_bytes, DecodeProcessLayout, EncodeProcessLayout, random);
        {
            auto bad = layout;
            bad.blocks[1].address = bad.blocks[0].address; // overlap
            Rejects([&] { (void)EncodeProcessLayout(bad); }, "overlapping blocks");
            bad = layout;
            bad.runs.clear();
            Rejects([&] { (void)EncodeProcessLayout(bad); }, "no content");
            bad = layout;
            bad.runs[0].pages = MaxContentPages + 1;
            Rejects([&] { (void)EncodeProcessLayout(bad); }, "content beyond the job limit");
            bad = layout;
            bad.aliases[0].size = 0x1001;
            Rejects([&] { (void)EncodeProcessLayout(bad); }, "unaligned stack mapping");
            bad = layout;
            bad.heap_start = ~u64{0} & ~(GuestPageSize - 1);
            Rejects([&] { (void)EncodeProcessLayout(bad); }, "wrapping heap");
            auto huge = layout_bytes;
            huge[128] = 0xff; // block count, after the 16 u64 header fields
            huge[129] = 0xff;
            huge[130] = 0xff;
            Rejects([&] { (void)DecodeProcessLayout(huge); }, "block count beyond the bytes");
        }

        const auto objects = MakeObjects();
        const auto object_bytes = EncodeObjects(objects);
        Require(DecodeObjects(object_bytes) == objects, "objects round trip");
        accepted += Mutate(object_bytes, DecodeObjects, [](const auto& r) { return EncodeObjects(r); }, random);
        {
            auto bad = objects;
            std::swap(bad[1], bad[2]); // readable before its event
            Rejects([&] { (void)EncodeObjects(bad); }, "readable event before its event");
            bad = objects;
            bad.erase(bad.begin() + 2); // event without readable
            Rejects([&] { (void)EncodeObjects(bad); }, "event without readable");
            bad = objects;
            bad[3].id = 5;
            Rejects([&] { (void)EncodeObjects(bad); }, "duplicate identity");
            bad = objects;
            bad[0].id = 9;
            Rejects([&] { (void)EncodeObjects(bad); }, "process with another identity");
            bad = objects;
            bad[3].payload.clear();
            Rejects([&] { (void)EncodeObjects(bad); }, "session without its graph");
            bad = objects;
            bad[1].flags = 1;
            Rejects([&] { (void)EncodeObjects(bad); }, "signal flag on a writable event");
            auto unknown = object_bytes;
            unknown[8 + 8] = 9; // kind of the first record
            Rejects([&] { (void)DecodeObjects(unknown); }, "unknown object kind");
        }

        const auto threads = MakeThreads(random);
        const auto thread_bytes = EncodeThreads(threads);
        Require(thread_bytes.size() == 8 + threads.size() * ThreadRecordSize, "thread record size");
        Require(DecodeThreads(thread_bytes) == threads, "threads round trip");
        accepted += Mutate(thread_bytes, DecodeThreads, [](const auto& r) { return EncodeThreads(r); }, random);
        {
            auto bad = threads;
            bad[1].thread_id = bad[0].thread_id;
            Rejects([&] { (void)EncodeThreads(bad); }, "duplicate guest thread ID");
            bad = threads;
            bad[1].tls = bad[0].tls;
            Rejects([&] { (void)EncodeThreads(bad); }, "shared thread-local region");
            bad = threads;
            bad[0].base_priority = 64;
            Rejects([&] { (void)EncodeThreads(bad); }, "priority out of range");
            bad = threads;
            bad[0].affinity = 0x2;
            bad[0].ideal_core = 0;
            Rejects([&] { (void)EncodeThreads(bad); }, "ideal core outside the affinity");
            bad = threads;
            bad[0].affinity = 0x10;
            Rejects([&] { (void)EncodeThreads(bad); }, "affinity beyond four cores");
            Rejects([&] { (void)EncodeThreads({}); }, "process without threads");
        }

        const auto waits = MakeWaits();
        const auto wait_bytes = EncodeWaits(waits);
        const auto decoded_waits = DecodeWaits(wait_bytes);
        Require(decoded_waits.size() == waits.size() && EncodeWaits(decoded_waits) == wait_bytes,
                "waits round trip");
        accepted += Mutate(wait_bytes, DecodeWaits, [](const auto& r) { return EncodeWaits(r); }, random);
        {
            auto bad = waits;
            bad[0].wait.timer_ordinal = 0; // two waits claim the first timer slot
            const auto encoded = EncodeWaits(bad);
            Rejects([&] { (void)DecodeWaits(encoded); }, "duplicated timer ordinal");
            bad = waits;
            bad[1].id = bad[0].id;
            const auto duplicated = EncodeWaits(bad);
            Rejects([&] { (void)DecodeWaits(duplicated); }, "one thread waiting twice");
        }

        const auto timing = MakeTiming();
        const auto timing_bytes = EncodeTiming(timing);
        Require(DecodeTiming(timing_bytes) == timing, "timing round trip");
        accepted += Mutate(timing_bytes, DecodeTiming, EncodeTiming, random);
        {
            auto bad = timing;
            bad.bindings[1].name = bad.bindings[0].name;
            Rejects([&] { (void)EncodeTiming(bad); }, "duplicate event name");
            bad = timing;
            bad.events[0].binding_id = 2;
            Rejects([&] { (void)EncodeTiming(bad); }, "event without binding");
            bad = timing;
            bad.events[1].fifo_order = 39;
            Rejects([&] { (void)EncodeTiming(bad); }, "duplicate FIFO order");
            bad = timing;
            bad.pause_end_time = bad.time_ns + 1;
            Rejects([&] { (void)EncodeTiming(bad); }, "pause after now");
            bad = timing;
            bad.bindings[0].name.assign(MaxTimingNameLength + 1, 'x');
            Rejects([&] { (void)EncodeTiming(bad); }, "event name too long");
        }

        std::cout << "pulse process sections: round trips, " << accepted
                  << " benign corruptions re-encoded exactly, all truncations and inconsistencies"
                     " rejected PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
