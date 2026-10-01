// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

// Staging references identify dedicated buffers by unique ID, never by their vector position.
// Swap removal bounds CPU work even when a size bucket contains thousands of busy buffers.
namespace D3D12 {
template <class Entries, class IsRetired>
bool ReclaimRetiredStaging(Entries& entries, std::size_t& cursor, IsRetired&& retired) {
    bool changed = false;
    if (cursor >= entries.size()) {
        cursor = 0;
    }
    for (unsigned checked = 0; checked < 16 && cursor < entries.size(); ++checked) {
        if (retired(entries[cursor])) {
            if (cursor + 1 != entries.size()) {
                entries[cursor] = std::move(entries.back());
            }
            entries.pop_back();
            changed = true;
        } else {
            ++cursor;
        }
    }
    if (cursor >= entries.size()) {
        cursor = 0;
    }
    return changed;
}
} // namespace D3D12

namespace D3D12 {

/// Recording-thread policy. Budgets only shrink during a game, except that an emergency-disabled
/// ring may recover to 64 MiB after two seconds of healthy frames. No clocks or allocations.
class MemoryGuardPolicy {
public:
    static constexpr std::uint64_t MiB = 1024 * 1024;
    struct Decision {
        std::uint64_t stream_bytes;
        bool trim;
        bool emergency;
    };

    Decision Update(std::uint64_t used, std::uint64_t limit) {
        if (!limit) {
            healthy_frames = 0;
            return {target, false, false};
        }
        const auto free = used >= limit ? 0 : limit - used;
        const bool emergency = free <= 10 * MiB;
        if (emergency) {
            target = 0;
        } else if (free <= 64 * MiB) {
            target = target > 64 * MiB ? 64 * MiB : target;
        } else if (free <= 128 * MiB) {
            target = target > 128 * MiB ? 128 * MiB : target;
        }
        if (free >= 256 * MiB && target == 0) {
            if (++healthy_frames >= 120) {
                target = 64 * MiB;
                healthy_frames = 0;
            }
        } else {
            healthy_frames = 0;
        }
        return {target, free <= 128 * MiB, emergency};
    }

private:
    std::uint64_t target = 256 * MiB;
    unsigned healthy_frames{};
};

} // namespace D3D12
