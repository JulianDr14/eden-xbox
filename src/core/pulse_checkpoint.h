// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/pulse_archive.h"
#include "core/hle/kernel/pulse_process_layout.h"
#include "core/pulse_process_state.h"

namespace Core {
class System;
}
namespace Kernel {
class KAutoObject;
class KProcess;
} // namespace Kernel

namespace Core::Pulse {

// Archive sections written by the process coordinator, in file order. Gaps leave room for the
// service, filesystem, audio and GPU serializers, which are separate sections.
enum class ProcessSection : u32 {
    Process = 0x10,   // identity, layout pin, memory map, alias mappings, content runs
    Objects = 0x20,   // kernel objects the guest can reach (events, sm sessions)
    Threads = 0x30,   // guest threads: IDs, TLS, priorities, affinity, ARM context
    Memory = 0x40,    // guest memory contents, one copy per physical page
    Handles = 0x50,   // the process handle table
    Waits = 0x60,     // blocked SVCs as explicit continuations
    Exclusive = 0x70, // exclusive monitor reservations
    Timing = 0x80,    // CoreTiming queue and clocks
};
inline constexpr u32 ProcessSectionVersion = 2;

// The system is in a state that cannot be captured. Retryable when the cause is transient (a
// thread inside an SVC or an IPC round trip): run the guest briefly and capture again. Not
// retryable when an object has no serializer: fail closed, the state is never written.
class CheckpointRejected final : public std::runtime_error {
public:
    CheckpointRejected(bool retryable_, const std::string& reason)
        : std::runtime_error{reason}, retryable{retryable_} {}
    const bool retryable;
};

struct CheckpointStats {
    u32 threads{};
    u32 waiting_threads{};
    u32 objects{};
    u32 handles{};
    u32 memory_blocks{};
    u64 memory_bytes{};   // guest memory written to the archive (deduplicated physical pages)
    u64 metadata_bytes{}; // every other section
    u32 timing_events{};
};

// A captured process. Capture takes only metadata; Write streams guest memory straight from the
// emulated RAM, so the system must stay paused from Capture until Write returns.
class ProcessCheckpoint final {
public:
    // System paused; the application process is a 64-bit process created from metadata.
    static ProcessCheckpoint Capture(Core::System& system);

    // The system running: raises the kernel checkpoint barrier, so threads entering an SVC park
    // before the call, and captures as soon as the in-flight calls have drained. Returns with
    // the system paused. Throws the last rejection once `budget` runs out or one is final, with
    // the system running again. `retries` counts the rejected attempts.
    static ProcessCheckpoint CaptureAtBarrier(Core::System& system,
                                              std::chrono::milliseconds budget, u32& retries);

    void Write(Common::Pulse::Writer& writer) const;
    [[nodiscard]] u64 DecodedSize() const;
    // Bound on the archive bytes Write produces (stored blocks never exceed their raw size).
    [[nodiscard]] u64 MaxEncodedSize() const;
    [[nodiscard]] const CheckpointStats& Stats() const {
        return stats;
    }

private:
    struct SectionBytes {
        ProcessSection id;
        std::vector<u8> bytes;
    };
    Kernel::KProcess* process{};
    std::vector<SectionBytes> sections; // every section but Memory
    std::vector<ContentRun> runs;
    std::vector<u64> run_ends; // prefix sums of the runs, in bytes
    u64 memory_size{};
    CheckpointStats stats;
};

// Rebuilds a captured process into a fresh, stopped System. load(pin) must load the same program
// with kernel.PinNextProcessLayout(pin) set, without running it (Service::Process::Run skips a
// process that is restoring). Feed every section with Consume, in archive order; the last one
// finishes the rebuild (Finish checks that it happened). The caller then runs the system. Any
// exception: discard the System, never run it.
class ProcessRestore final {
public:
    using LoadSkeleton = std::function<void(const Kernel::ProcessLayoutPin&)>;
    ProcessRestore(Core::System& system, LoadSkeleton load);
    ~ProcessRestore();
    ProcessRestore(const ProcessRestore&) = delete;
    ProcessRestore& operator=(const ProcessRestore&) = delete;

    void Consume(Common::Pulse::Section section, u64 offset, std::span<const u8> data);
    void Finish();
    [[nodiscard]] const CheckpointStats& Stats() const {
        return stats;
    }

    struct State; // decoded metadata, internal

private:
    void Complete(ProcessSection id, std::span<const u8> bytes);
    void VerifyLayout();

    Core::System& system;
    LoadSkeleton load;
    std::unique_ptr<State> state;
    Kernel::KProcess* process{};
    u32 next_section{};
    std::vector<u8> buffer;
    bool layout_verified{};
    bool finished{};
    CheckpointStats stats;
};

} // namespace Core::Pulse
