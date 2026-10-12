// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>
#include "common/common_types.h"

namespace Kernel {
class KAutoObject;
using IdentifyCheckpointObject = std::function<u64(const KAutoObject*)>;
using ResolveCheckpointObject = std::function<KAutoObject*(u64)>;

struct CheckpointHandleEntry {
    u64 object{};
    u16 linear_id{};
    s16 next_free{-1};
};
struct HandleTableCheckpoint {
    u16 next_linear_id{};
    u16 max_count{};
    s16 free_head{-1};
    std::vector<CheckpointHandleEntry> entries;
};

inline void ValidateHandleTableCheckpoint(const HandleTableCheckpoint& state) {
    if (state.entries.empty() || state.entries.size() > 1024 || !state.next_linear_id ||
        state.next_linear_id > 0x7fff || state.max_count > state.entries.size() ||
        state.free_head < -1 || state.free_head >= static_cast<s32>(state.entries.size())) {
        throw std::invalid_argument("Pulse: invalid handle table header");
    }
    size_t occupied{};
    for (const auto& entry : state.entries) {
        if ((entry.object && (!entry.linear_id || entry.linear_id > 0x7fff || entry.next_free != -1)) ||
            (!entry.object && (entry.linear_id || entry.next_free < -1 ||
                              entry.next_free >= static_cast<s32>(state.entries.size())))) {
            throw std::invalid_argument("Pulse: invalid handle table entry");
        }
        occupied += entry.object != 0;
    }
    if (occupied > state.max_count) throw std::invalid_argument("Pulse: invalid handle high water mark");
    std::array<bool, 1024> visited{};
    size_t free_count{};
    for (s32 index = state.free_head; index != -1; index = state.entries[index].next_free) {
        if (visited[index] || state.entries[index].object) {
            throw std::invalid_argument("Pulse: cyclic or occupied handle free list");
        }
        visited[index] = true;
        ++free_count;
    }
    if (free_count + occupied != state.entries.size()) {
        throw std::invalid_argument("Pulse: reserved or missing handle table slot");
    }
}

inline std::vector<u8> EncodeHandleTableCheckpoint(const HandleTableCheckpoint& state) {
    ValidateHandleTableCheckpoint(state);
    std::vector<u8> bytes(16 + state.entries.size() * 16);
    size_t offset{};
    const auto put = [&](u64 value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes[offset++] = static_cast<u8>(value >> (i * 8));
    };
    put(1, 4); put(state.entries.size(), 2); put(state.next_linear_id, 2);
    put(state.max_count, 2); put(static_cast<u16>(state.free_head), 2); put(0, 4);
    for (const auto& entry : state.entries) {
        put(entry.object, 8); put(entry.linear_id, 2); put(static_cast<u16>(entry.next_free), 2); put(0, 4);
    }
    return bytes;
}

inline HandleTableCheckpoint DecodeHandleTableCheckpoint(std::span<const u8> bytes) {
    if (bytes.size() < 16 || bytes.size() > 16 + 1024 * 16) {
        throw std::invalid_argument("Pulse: handle table section size");
    }
    size_t offset{};
    const auto get = [&](size_t width) {
        u64 value{};
        for (size_t i = 0; i < width; ++i) value |= u64{bytes[offset++]} << (i * 8);
        return value;
    };
    const auto version = get(4);
    const auto count = get(2);
    const auto next = get(2);
    const auto max = get(2);
    const auto head = get(2);
    if (get(4) || version != 1 || !count || count > 1024 || bytes.size() != 16 + count * 16) {
        throw std::invalid_argument("Pulse: malformed handle table section");
    }
    HandleTableCheckpoint state{static_cast<u16>(next), static_cast<u16>(max),
                                static_cast<s16>(static_cast<u16>(head)), {}};
    state.entries.reserve(static_cast<size_t>(count));
    for (u64 i = 0; i < count; ++i) {
        const auto id = get(8);
        const auto linear = get(2);
        const auto free = get(2);
        if (get(4)) throw std::invalid_argument("Pulse: handle entry reserved bits");
        state.entries.push_back({id, static_cast<u16>(linear), static_cast<s16>(static_cast<u16>(free))});
    }
    ValidateHandleTableCheckpoint(state);
    return state;
}
} // namespace Kernel
