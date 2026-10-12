// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
#include "common/common_types.h"

namespace Kernel {
class KernelCore;
class KThread;
class KThreadQueue;
class KSynchronizationObject;

enum class CheckpointWaitKind : u8 { None, Sleep, Synchronization, AddressArbiter, Mutex, ConditionVariable };

struct WaitObjectReference {
    u64 id{};
    u64 ordinal{};
    bool operator==(const WaitObjectReference&) const = default;
};

struct ThreadWaitCheckpoint {
    CheckpointWaitKind kind{};
    // In the restored virtual nanosecond timeline, never a host wall clock.
    s64 deadline{};
    std::vector<WaitObjectReference> objects;
    u64 address{};
    u64 ordinal{};
    u64 condition_key{};
    u64 reference_owner{};
    u32 tag{};
    u64 timer_ordinal{};
};

// Assign IDs from the explicit checkpoint graph registry. KAutoObject::GetId()
// is unsuitable: its default implementation returns a host address.
using IdentifyWaitObject = std::function<u64(const KSynchronizationObject*)>;
using ResolveWaitObject = std::function<KSynchronizationObject*(u64)>;

inline void ValidateWaitCheckpoint(const ThreadWaitCheckpoint& state) {
    const bool mutex = state.kind == CheckpointWaitKind::Mutex;
    const bool condition = state.kind == CheckpointWaitKind::ConditionVariable;
    const bool lock = mutex || condition;
    const bool address = state.kind == CheckpointWaitKind::AddressArbiter || lock;
    if (state.deadline < 0 || state.objects.size() > 64 || state.timer_ordinal >= 65536 ||
        (!state.deadline && state.timer_ordinal) ||
        state.kind < CheckpointWaitKind::Sleep || state.kind > CheckpointWaitKind::ConditionVariable ||
        (state.kind == CheckpointWaitKind::Sleep &&
         (!state.objects.empty() || state.deadline == 0)) ||
        (address && (!state.address || (state.address & 3) || state.ordinal >= 65536)) ||
        (!address && (state.address || state.ordinal)) ||
        (state.kind == CheckpointWaitKind::AddressArbiter && !state.objects.empty()) ||
        (lock && state.objects.size() > 1) ||
        (mutex && (state.objects.size() != 1 || !state.reference_owner || state.deadline ||
                   state.condition_key || state.ordinal)) ||
        (condition && (!state.condition_key || (state.condition_key & 3) || state.reference_owner ||
                       (!state.objects.empty() && state.ordinal))) ||
        (!lock && (state.condition_key || state.reference_owner || state.tag))) {
        throw std::invalid_argument("Pulse: invalid kernel wait checkpoint");
    }
    for (auto reference : state.objects) {
        if (!reference.id || reference.ordinal >= 65536) {
            throw std::invalid_argument("Pulse: invalid synchronization object reference");
        }
    }
}

inline std::vector<u8> EncodeWaitCheckpoint(const ThreadWaitCheckpoint& state) {
    ValidateWaitCheckpoint(state);
    std::vector<u8> bytes(64 + state.objects.size() * 16);
    size_t offset{};
    const auto put = [&](u64 value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes[offset++] = static_cast<u8>(value >> (i * 8));
    };
    put(static_cast<u8>(state.kind), 4);
    put(state.objects.size(), 4);
    put(static_cast<u64>(state.deadline), 8);
    put(state.address, 8);
    put(state.ordinal, 8);
    put(state.condition_key, 8);
    put(state.reference_owner, 8);
    put(state.tag, 4);
    put(0, 4);
    put(state.timer_ordinal, 8);
    for (auto reference : state.objects) {
        put(reference.id, 8);
        put(reference.ordinal, 8);
    }
    return bytes;
}

inline ThreadWaitCheckpoint DecodeWaitCheckpoint(std::span<const u8> bytes) {
    if (bytes.size() < 64 || bytes.size() > 64 + 64 * 16) {
        throw std::invalid_argument("Pulse: kernel wait section size");
    }
    size_t offset{};
    const auto get = [&](size_t width) {
        u64 value{};
        for (size_t i = 0; i < width; ++i) value |= u64{bytes[offset++]} << (i * 8);
        return value;
    };
    const auto kind = get(4);
    const auto count = get(4);
    const auto deadline = get(8);
    const auto address = get(8);
    const auto ordinal = get(8);
    const auto key = get(8);
    const auto reference_owner = get(8);
    const auto tag = get(4);
    const auto reserved = get(4);
    const auto timer_ordinal = get(8);
    if (kind > static_cast<u8>(CheckpointWaitKind::ConditionVariable) || reserved || count > 64 ||
        bytes.size() != 64 + count * 16 || deadline > static_cast<u64>((std::numeric_limits<s64>::max)())) {
        throw std::invalid_argument("Pulse: malformed kernel wait section");
    }
    ThreadWaitCheckpoint state{static_cast<CheckpointWaitKind>(kind), static_cast<s64>(deadline), {}, address, ordinal, key, reference_owner, static_cast<u32>(tag), timer_ordinal};
    state.objects.reserve(static_cast<size_t>(count));
    for (u64 i = 0; i < count; ++i) state.objects.push_back({get(8), get(8)});
    ValidateWaitCheckpoint(state);
    return state;
}

// Reproduce the generated SVC ABI after the wait has completed on a new fiber.
// SleepThread has no output registers. Both ARM ABIs return synchronization
// result/index in r0/r1 and the arbiter result in r0, with zero extension.
inline void CompleteWaitArguments(CheckpointWaitKind kind, u32 result, s32 index,
                                  std::span<u64, 8> arguments) {
    switch (kind) {
    case CheckpointWaitKind::Sleep:
        return;
    case CheckpointWaitKind::Synchronization:
        arguments[0] = result;
        arguments[1] = static_cast<u32>(index);
        return;
    case CheckpointWaitKind::Mutex:
    case CheckpointWaitKind::ConditionVariable:
    case CheckpointWaitKind::AddressArbiter:
        arguments[0] = result;
        return;
    default:
        throw std::invalid_argument("Pulse: unsupported wait continuation");
    }
}

// Resolves all references and owns fresh list nodes. Linking happens separately,
// after the entire object graph exists and validation has succeeded.
std::unique_ptr<KThreadQueue> CreateCheckpointSynchronizationWait(
    KernelCore& kernel, const ThreadWaitCheckpoint& state, const ResolveWaitObject& resolve);
} // namespace Kernel
