// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wire format of the process coordinator's sections (pulse_checkpoint.h): explicit little-endian
// records, validated on decode before anything touches a fresh System. Header-only so that the
// malformed-input gate (tools/xbox/tests/pulse-process.cpp) runs without the kernel.

#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/common_types.h"
#include "common/pulse_archive.h"
#include "core/hle/kernel/pulse_process_layout.h"
#include "core/hle/kernel/pulse_wait_graph.h"
#include "core/pulse_machine_state.h"

namespace Core::Pulse {

inline constexpr u64 GuestPageSize = 0x1000;
inline constexpr size_t MaxMemoryBlocks = 65536;
inline constexpr size_t MaxStackAliases = 4096;
inline constexpr size_t MaxContentRuns = 65536;
inline constexpr size_t MaxKernelObjects = 65536;
inline constexpr size_t MaxGuestThreads = 1024;
inline constexpr size_t MaxTimingNames = 256;
inline constexpr size_t MaxTimingNameLength = 128;
inline constexpr size_t MaxTimingEvents = 65536;
inline constexpr u64 MaxContentPages = (1ULL << 40) / GuestPageSize;

namespace Wire {
[[noreturn]] inline void Invalid(const char* reason) {
    throw Common::Pulse::Failure{Common::Pulse::Error::Invalid, reason};
}
struct Out {
    std::vector<u8> bytes;
    void Put(u64 value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes.push_back(static_cast<u8>(value >> (i * 8)));
    }
    void Append(std::span<const u8> data) {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
};
struct In {
    std::span<const u8> bytes;
    size_t offset{};
    u64 Get(size_t width) {
        if (width > bytes.size() - offset) Invalid("Pulse: truncated process section");
        u64 value{};
        for (size_t i = 0; i < width; ++i) value |= u64{bytes[offset++]} << (i * 8);
        return value;
    }
    std::span<const u8> Take(u64 size) {
        if (size > bytes.size() - offset) Invalid("Pulse: truncated process section");
        const auto data = bytes.subspan(offset, static_cast<size_t>(size));
        offset += static_cast<size_t>(size);
        return data;
    }
    void Reserved(size_t width) {
        if (Get(width)) Invalid("Pulse: process section reserved bits");
    }
    void End() const {
        if (offset != bytes.size()) Invalid("Pulse: trailing bytes in process section");
    }
};
} // namespace Wire

// ---------------------------------------------------------------- section Process

struct MemoryBlock {
    u64 address{};
    u64 size{};
    u32 state{};
    u32 permission{};
    u32 attribute{};
    bool operator==(const MemoryBlock&) const = default;
};
struct StackAlias {
    u64 destination{};
    u64 source{};
    u64 size{};
    bool operator==(const StackAlias&) const = default;
};
struct ContentRun {
    u64 address{};
    u64 pages{};
    bool operator==(const ContentRun&) const = default;
};
struct ProcessLayout {
    u64 program_id{};
    u64 process_id{};
    Kernel::ProcessLayoutPin pin{};
    u64 main_stack_address{};
    u64 main_stack_size{};
    u64 heap_start{};
    u64 heap_size{};
    std::vector<MemoryBlock> blocks;  // sorted, non-overlapping, coalesced
    std::vector<StackAlias> aliases;  // MapMemory(destination, source, size), in order
    std::vector<ContentRun> runs;     // pages stored in section Memory, in order
    bool operator==(const ProcessLayout&) const = default;

    [[nodiscard]] u64 ContentSize() const {
        u64 total{};
        for (const auto& run : runs) total += run.pages * GuestPageSize;
        return total;
    }
};

inline void ValidateProcessLayout(const ProcessLayout& s) {
    using Wire::Invalid;
    if (s.blocks.size() > MaxMemoryBlocks || s.aliases.size() > MaxStackAliases ||
        s.runs.empty() || s.runs.size() > MaxContentRuns ||
        ((s.main_stack_address | s.main_stack_size | s.heap_start | s.heap_size) & (GuestPageSize - 1)) ||
        s.main_stack_address + s.main_stack_size < s.main_stack_address ||
        s.heap_start + s.heap_size < s.heap_start) {
        Invalid("Pulse: malformed process layout");
    }
    u64 end{};
    for (const auto& block : s.blocks) {
        if (!block.size || ((block.address | block.size) & (GuestPageSize - 1)) ||
            block.address + block.size < block.address || block.address < end) {
            Invalid("Pulse: malformed memory block");
        }
        end = block.address + block.size;
    }
    for (const auto& alias : s.aliases) {
        if (!alias.size || ((alias.destination | alias.source | alias.size) & (GuestPageSize - 1)) ||
            alias.destination + alias.size < alias.destination || alias.source + alias.size < alias.source) {
            Invalid("Pulse: malformed stack mapping");
        }
    }
    u64 pages{};
    end = 0;
    for (const auto& run : s.runs) {
        if (!run.pages || run.pages > MaxContentPages || (run.address & (GuestPageSize - 1)) ||
            run.address + run.pages * GuestPageSize < run.address || pages > MaxContentPages - run.pages) {
            Invalid("Pulse: malformed memory run");
        }
        pages += run.pages;
    }
}

inline std::vector<u8> EncodeProcessLayout(const ProcessLayout& s) {
    ValidateProcessLayout(s);
    Wire::Out out;
    out.Put(s.program_id, 8);
    out.Put(s.process_id, 8);
    out.Put(s.pin.aslr_offset, 8);
    for (auto value : s.pin.region_random) out.Put(value, 8);
    out.Put(s.pin.plr_address, 8);
    for (auto value : s.pin.entropy) out.Put(value, 8);
    out.Put(s.main_stack_address, 8);
    out.Put(s.main_stack_size, 8);
    out.Put(s.heap_start, 8);
    out.Put(s.heap_size, 8);
    out.Put(s.blocks.size(), 4);
    out.Put(s.aliases.size(), 4);
    out.Put(s.runs.size(), 4);
    out.Put(0, 4);
    for (const auto& block : s.blocks) {
        out.Put(block.address, 8);
        out.Put(block.size, 8);
        out.Put(block.state, 4);
        out.Put(block.permission, 4);
        out.Put(block.attribute, 4);
        out.Put(0, 4);
    }
    for (const auto& alias : s.aliases) {
        out.Put(alias.destination, 8);
        out.Put(alias.source, 8);
        out.Put(alias.size, 8);
    }
    for (const auto& run : s.runs) {
        out.Put(run.address, 8);
        out.Put(run.pages, 8);
    }
    return std::move(out.bytes);
}

inline ProcessLayout DecodeProcessLayout(std::span<const u8> bytes) {
    Wire::In in{bytes};
    ProcessLayout s;
    s.program_id = in.Get(8);
    s.process_id = in.Get(8);
    s.pin.aslr_offset = in.Get(8);
    for (auto& value : s.pin.region_random) value = in.Get(8);
    s.pin.plr_address = in.Get(8);
    for (auto& value : s.pin.entropy) value = in.Get(8);
    s.main_stack_address = in.Get(8);
    s.main_stack_size = in.Get(8);
    s.heap_start = in.Get(8);
    s.heap_size = in.Get(8);
    const auto block_count = in.Get(4);
    const auto alias_count = in.Get(4);
    const auto run_count = in.Get(4);
    in.Reserved(4);
    // Counts are checked against the bytes present before anything is reserved.
    if (block_count > MaxMemoryBlocks || alias_count > MaxStackAliases || run_count > MaxContentRuns ||
        block_count * 32 + alias_count * 24 + run_count * 16 != bytes.size() - in.offset) {
        Wire::Invalid("Pulse: malformed process layout");
    }
    s.blocks.reserve(static_cast<size_t>(block_count));
    for (u64 i = 0; i < block_count; ++i) {
        MemoryBlock block{};
        block.address = in.Get(8);
        block.size = in.Get(8);
        block.state = static_cast<u32>(in.Get(4));
        block.permission = static_cast<u32>(in.Get(4));
        block.attribute = static_cast<u32>(in.Get(4));
        in.Reserved(4);
        s.blocks.push_back(block);
    }
    for (u64 i = 0; i < alias_count; ++i) {
        StackAlias alias{};
        alias.destination = in.Get(8);
        alias.source = in.Get(8);
        alias.size = in.Get(8);
        s.aliases.push_back(alias);
    }
    for (u64 i = 0; i < run_count; ++i) {
        ContentRun run{};
        run.address = in.Get(8);
        run.pages = in.Get(8);
        s.runs.push_back(run);
    }
    in.End();
    ValidateProcessLayout(s);
    return s;
}

// ---------------------------------------------------------------- section Objects

// Event: owned by the application. ServiceEvent: owned by a service, rebuilt by the interface that
// exported it. ServiceHandler: an HLE interface, not a kernel object, in the same identity space so
// session graphs and service events can reference it.
enum class ObjectKind : u32 {
    Process = 1,
    Event = 2,
    ReadableEvent = 3,
    ClientSession = 4,
    ServiceHandler = 5,
    ServiceEvent = 6,
    // A service's or the kernel's shared memory, by owner key, with the guest's mappings of it.
    SharedMemory = 7,
    // Application heap lent through svcCreateTransferMemory: address, size, owner permission.
    TransferMemory = 8,
};
inline constexpr size_t SharedMemoryPayloadHeader = 16; // u32 key kind, u32 count, u64 key argument
inline constexpr size_t SharedMappingSize = 20;         // u64 address, u64 size, u32 permission
inline constexpr size_t TransferMemoryPayload = 20;     // u64 address, u64 size, u32 permission
inline constexpr u64 ProcessObjectId = 1;
inline constexpr size_t MaxObjectPayload = (1U << 20) + 4096;

struct ObjectRecord {
    u64 id{};
    ObjectKind kind{};
    u32 flags{};  // ReadableEvent: bit 0 signaled
    u64 parent{}; // ReadableEvent: its (service) event; ServiceEvent: its ServiceHandler
    // ClientSession: Service::Pulse::EncodeSessionRecord; ServiceHandler: EncodeHandlerRecord;
    // ServiceEvent: the u32 slot its handler exported it under
    std::vector<u8> payload;
    bool operator==(const ObjectRecord&) const = default;
};

// IDs are unique and nonzero; a readable event follows its event; the process is ID 1.
inline void ValidateObjects(std::span<const ObjectRecord> records) {
    using Wire::Invalid;
    if (records.size() > MaxKernelObjects) Invalid("Pulse: too many kernel objects");
    std::unordered_set<u64> ids;
    std::unordered_set<u64> events;
    std::unordered_set<u64> handlers;
    std::vector<u64> handler_refs;
    for (const auto& r : records) {
        const bool with_payload = r.kind == ObjectKind::ClientSession ||
                                  r.kind == ObjectKind::ServiceHandler ||
                                  r.kind == ObjectKind::ServiceEvent ||
                                  r.kind == ObjectKind::SharedMemory ||
                                  r.kind == ObjectKind::TransferMemory;
        if (!r.id || !ids.insert(r.id).second || r.payload.size() > MaxObjectPayload ||
            r.flags > 1 || (r.flags && r.kind != ObjectKind::ReadableEvent) ||
            (r.parent && r.kind != ObjectKind::ReadableEvent && r.kind != ObjectKind::ServiceEvent) ||
            with_payload == r.payload.empty() ||
            (r.kind == ObjectKind::ServiceEvent && r.payload.size() != 4) ||
            (r.kind == ObjectKind::SharedMemory &&
             (r.payload.size() < SharedMemoryPayloadHeader ||
              (r.payload.size() - SharedMemoryPayloadHeader) % SharedMappingSize != 0)) ||
            (r.kind == ObjectKind::TransferMemory && r.payload.size() != TransferMemoryPayload) ||
            ((r.kind == ObjectKind::Process) != (r.id == ProcessObjectId))) {
            Invalid("Pulse: malformed kernel object record");
        }
        switch (r.kind) {
        case ObjectKind::Process:
        case ObjectKind::ClientSession:
        case ObjectKind::SharedMemory:
        case ObjectKind::TransferMemory:
            break;
        case ObjectKind::ServiceHandler:
            handlers.insert(r.id);
            break;
        case ObjectKind::ServiceEvent:
            handler_refs.push_back(r.parent);
            [[fallthrough]];
        case ObjectKind::Event:
            events.insert(r.id);
            break;
        case ObjectKind::ReadableEvent:
            if (!events.erase(r.parent)) Invalid("Pulse: readable event without its event");
            break;
        default:
            Invalid("Pulse: unknown kernel object kind");
        }
    }
    if (!events.empty()) Invalid("Pulse: event without its readable event");
    for (const auto id : handler_refs) {
        if (!handlers.contains(id)) Invalid("Pulse: service event without its interface");
    }
}

inline std::vector<u8> EncodeObjects(std::span<const ObjectRecord> records) {
    ValidateObjects(records);
    Wire::Out out;
    out.Put(records.size(), 4);
    out.Put(0, 4);
    for (const auto& r : records) {
        out.Put(r.id, 8);
        out.Put(static_cast<u32>(r.kind), 4);
        out.Put(r.flags, 4);
        out.Put(r.parent, 8);
        out.Put(r.payload.size(), 4);
        out.Put(0, 4);
        out.Append(r.payload);
    }
    return std::move(out.bytes);
}

inline std::vector<ObjectRecord> DecodeObjects(std::span<const u8> bytes) {
    Wire::In in{bytes};
    const auto count = in.Get(4);
    in.Reserved(4);
    if (count > MaxKernelObjects || count * 32 > bytes.size()) Wire::Invalid("Pulse: too many kernel objects");
    std::vector<ObjectRecord> records;
    records.reserve(static_cast<size_t>(count));
    for (u64 i = 0; i < count; ++i) {
        ObjectRecord r{};
        r.id = in.Get(8);
        r.kind = static_cast<ObjectKind>(in.Get(4));
        r.flags = static_cast<u32>(in.Get(4));
        r.parent = in.Get(8);
        const auto size = in.Get(4);
        in.Reserved(4);
        const auto payload = in.Take(size);
        r.payload.assign(payload.begin(), payload.end());
        records.push_back(std::move(r));
    }
    in.End();
    ValidateObjects(records);
    return records;
}

// ---------------------------------------------------------------- section Threads

enum class ThreadRun : u32 { Initialized = 0, Runnable = 1, Waiting = 2 };

struct ThreadRecord {
    u64 id{};        // graph identity
    u64 thread_id{}; // guest thread ID
    u64 tls{};
    ThreadRun run{};
    u32 flags{}; // bit 0: paused by the guest (svcSetThreadActivity)
    s32 base_priority{};
    s32 ideal_core{};
    u64 affinity{};
    EncodedContext context{};
    bool operator==(const ThreadRecord&) const = default;
};
inline constexpr size_t ThreadRecordSize = 56 + ContextSize;

inline void ValidateThreads(std::span<const ThreadRecord> records) {
    if (records.empty() || records.size() > MaxGuestThreads) Wire::Invalid("Pulse: thread count");
    std::unordered_set<u64> ids, thread_ids, tls;
    for (const auto& r : records) {
        if (r.run > ThreadRun::Waiting || r.flags > 1 || !r.id || !r.thread_id || !r.tls ||
            (r.tls & 0x1ff) || r.base_priority < 0 || r.base_priority > 63 || r.ideal_core < -1 ||
            r.ideal_core > 3 || !r.affinity || (r.affinity >> 4) ||
            (r.ideal_core >= 0 && !((r.affinity >> r.ideal_core) & 1)) ||
            !ids.insert(r.id).second || !thread_ids.insert(r.thread_id).second ||
            !tls.insert(r.tls).second) {
            Wire::Invalid("Pulse: malformed thread record");
        }
    }
}

inline std::vector<u8> EncodeThreads(std::span<const ThreadRecord> records) {
    ValidateThreads(records);
    Wire::Out out;
    out.Put(records.size(), 4);
    out.Put(0, 4);
    for (const auto& r : records) {
        out.Put(r.id, 8);
        out.Put(r.thread_id, 8);
        out.Put(r.tls, 8);
        out.Put(static_cast<u32>(r.run), 4);
        out.Put(r.flags, 4);
        out.Put(static_cast<u32>(r.base_priority), 4);
        out.Put(static_cast<u32>(r.ideal_core), 4);
        out.Put(r.affinity, 8);
        out.Put(0, 8);
        out.Append(r.context);
    }
    return std::move(out.bytes);
}

inline std::vector<ThreadRecord> DecodeThreads(std::span<const u8> bytes) {
    Wire::In in{bytes};
    const auto count = in.Get(4);
    in.Reserved(4);
    if (!count || count > MaxGuestThreads || count * ThreadRecordSize != bytes.size() - 8) {
        Wire::Invalid("Pulse: thread count");
    }
    std::vector<ThreadRecord> records;
    for (u64 i = 0; i < count; ++i) {
        ThreadRecord r{};
        r.id = in.Get(8);
        r.thread_id = in.Get(8);
        r.tls = in.Get(8);
        r.run = static_cast<ThreadRun>(in.Get(4));
        r.flags = static_cast<u32>(in.Get(4));
        r.base_priority = static_cast<s32>(in.Get(4));
        r.ideal_core = static_cast<s32>(in.Get(4));
        r.affinity = in.Get(8);
        in.Reserved(8);
        const auto context = in.Take(ContextSize);
        std::copy(context.begin(), context.end(), r.context.begin());
        (void)DecodeContext(context);
        records.push_back(r);
    }
    in.End();
    ValidateThreads(records);
    return records;
}

// ---------------------------------------------------------------- section Waits

inline std::vector<u8> EncodeWaits(std::span<const Kernel::CheckpointWaitingThread> waits) {
    if (waits.size() > MaxGuestThreads) Wire::Invalid("Pulse: waiting thread count");
    Wire::Out out;
    out.Put(waits.size(), 4);
    out.Put(0, 4);
    for (const auto& wait : waits) {
        const auto encoded = Kernel::EncodeWaitCheckpoint(wait.wait);
        out.Put(wait.id, 8);
        out.Put(wait.process_id, 8);
        out.Put(static_cast<u32>(wait.effective_priority), 4);
        out.Put(encoded.size(), 4);
        out.Append(encoded);
    }
    return std::move(out.bytes);
}

// Also checks the ordering constraints (PlanCheckpointWaitRestore) before any kernel work.
inline std::vector<Kernel::CheckpointWaitingThread> DecodeWaits(std::span<const u8> bytes) {
    Wire::In in{bytes};
    const auto count = in.Get(4);
    in.Reserved(4);
    if (count > MaxGuestThreads || count * 88 > bytes.size()) Wire::Invalid("Pulse: waiting thread count");
    std::vector<Kernel::CheckpointWaitingThread> waits;
    for (u64 i = 0; i < count; ++i) {
        Kernel::CheckpointWaitingThread wait{};
        wait.id = in.Get(8);
        wait.process_id = in.Get(8);
        wait.effective_priority = static_cast<s32>(in.Get(4));
        const auto size = in.Get(4);
        try {
            wait.wait = Kernel::DecodeWaitCheckpoint(in.Take(size));
        } catch (const std::invalid_argument&) {
            Wire::Invalid("Pulse: malformed kernel wait");
        }
        waits.push_back(std::move(wait));
    }
    in.End();
    try {
        (void)Kernel::PlanCheckpointWaitRestore(waits);
    } catch (const std::invalid_argument&) {
        Wire::Invalid("Pulse: inconsistent wait graph");
    }
    return waits;
}

// ---------------------------------------------------------------- section Timing

struct TimingEventRecord {
    u64 binding_id{};
    s64 time_ns{};
    u64 fifo_order{};
    s64 period_ns{};
    bool operator==(const TimingEventRecord&) const = default;
};
struct TimingBindingRecord {
    u64 id{};
    std::string name;
    bool operator==(const TimingBindingRecord&) const = default;
};
struct TimingSection {
    bool multicore{};
    s64 time_ns{};
    u64 clock_ticks{};
    u64 gpu_ticks{};
    u64 cpu_ticks{};
    s64 downcount{};
    s64 pause_end_time{};
    u64 next_fifo_id{};
    std::vector<TimingBindingRecord> bindings; // by event name, never by host address
    std::vector<TimingEventRecord> events;
    bool operator==(const TimingSection&) const = default;
};

inline void ValidateTiming(const TimingSection& s) {
    using Wire::Invalid;
    if (s.time_ns < 0 || s.pause_end_time < 0 || s.pause_end_time > s.time_ns ||
        s.bindings.size() > MaxTimingNames || s.events.size() > MaxTimingEvents ||
        s.next_fifo_id >= (std::numeric_limits<u64>::max)() - MaxTimingEvents) {
        Invalid("Pulse: malformed timing section");
    }
    std::unordered_set<u64> ids;
    std::unordered_set<std::string> names;
    for (const auto& b : s.bindings) {
        if (!b.id || b.name.empty() || b.name.size() > MaxTimingNameLength ||
            !ids.insert(b.id).second || !names.insert(b.name).second) {
            Invalid("Pulse: malformed timing binding");
        }
    }
    std::unordered_set<u64> orders;
    for (const auto& e : s.events) {
        if (!ids.contains(e.binding_id) || e.time_ns < 0 || e.period_ns < 0 ||
            e.fifo_order >= s.next_fifo_id || !orders.insert(e.fifo_order).second) {
            Invalid("Pulse: malformed timing event");
        }
    }
}

inline std::vector<u8> EncodeTiming(const TimingSection& s) {
    ValidateTiming(s);
    Wire::Out out;
    out.Put(s.multicore ? 1 : 0, 4);
    out.Put(0, 4);
    out.Put(static_cast<u64>(s.time_ns), 8);
    out.Put(s.clock_ticks, 8);
    out.Put(s.gpu_ticks, 8);
    out.Put(s.cpu_ticks, 8);
    out.Put(static_cast<u64>(s.downcount), 8);
    out.Put(static_cast<u64>(s.pause_end_time), 8);
    out.Put(s.next_fifo_id, 8);
    out.Put(s.bindings.size(), 4);
    out.Put(s.events.size(), 4);
    for (const auto& b : s.bindings) {
        out.Put(b.id, 8);
        out.Put(b.name.size(), 4);
        out.Put(0, 4);
        out.Append({reinterpret_cast<const u8*>(b.name.data()), b.name.size()});
    }
    for (const auto& e : s.events) {
        out.Put(e.binding_id, 8);
        out.Put(static_cast<u64>(e.time_ns), 8);
        out.Put(e.fifo_order, 8);
        out.Put(static_cast<u64>(e.period_ns), 8);
    }
    return std::move(out.bytes);
}

inline TimingSection DecodeTiming(std::span<const u8> bytes) {
    Wire::In in{bytes};
    TimingSection s;
    const auto multicore = in.Get(4);
    in.Reserved(4);
    if (multicore > 1) Wire::Invalid("Pulse: timing flags");
    s.multicore = multicore != 0;
    s.time_ns = static_cast<s64>(in.Get(8));
    s.clock_ticks = in.Get(8);
    s.gpu_ticks = in.Get(8);
    s.cpu_ticks = in.Get(8);
    s.downcount = static_cast<s64>(in.Get(8));
    s.pause_end_time = static_cast<s64>(in.Get(8));
    s.next_fifo_id = in.Get(8);
    const auto binding_count = in.Get(4);
    const auto event_count = in.Get(4);
    if (binding_count > MaxTimingNames || event_count > MaxTimingEvents ||
        binding_count * 16 + event_count * 32 > bytes.size() - in.offset) {
        Wire::Invalid("Pulse: malformed timing section");
    }
    for (u64 i = 0; i < binding_count; ++i) {
        TimingBindingRecord b{};
        b.id = in.Get(8);
        const auto length = in.Get(4);
        in.Reserved(4);
        if (length > MaxTimingNameLength) Wire::Invalid("Pulse: timing event name");
        const auto name = in.Take(length);
        b.name.assign(name.begin(), name.end());
        s.bindings.push_back(std::move(b));
    }
    for (u64 i = 0; i < event_count; ++i) {
        TimingEventRecord e{};
        e.binding_id = in.Get(8);
        e.time_ns = static_cast<s64>(in.Get(8));
        e.fifo_order = in.Get(8);
        e.period_ns = static_cast<s64>(in.Get(8));
        s.events.push_back(e);
    }
    in.End();
    ValidateTiming(s);
    return s;
}

} // namespace Core::Pulse
