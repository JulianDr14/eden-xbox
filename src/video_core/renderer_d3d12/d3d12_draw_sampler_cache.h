// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "common/common_types.h"

namespace D3D12 {

/// Scratch space for one uninterrupted descriptor-gather pass, never retained by a pipeline.
/// A collision replaces an entry; equality uses the entire guest TSC index, not its slot.
/// The resolver must be discarded before any guest command can modify the TSC table.
template <typename Id>
class DrawSamplerCache {
public:
    explicit DrawSamplerCache(bool enabled_) : enabled{enabled_} {}

    template <typename Resolve>
    Id Get(u32 index, Resolve&& resolve) {
        if (!enabled) return resolve(index);
        const u32 slot = index & 3U;
        if ((valid & (1U << slot)) && entries[slot].index == index) {
            ++hits;
            return entries[slot].id;
        }
        const Id id = resolve(index);
        entries[slot] = {index, id};
        valid |= 1U << slot;
        return id;
    }

    [[nodiscard]] u32 Hits() const noexcept { return hits; }

private:
    struct Entry { u32 index{}; Id id{}; };
    std::array<Entry, 4> entries{};
    u32 valid{};
    u32 hits{};
    bool enabled{};
};

/// Experimental draw-local TSC resolution, disabled until gameplay A/B completes.
void SetDrawSamplerReuse(bool enabled);
bool DrawSamplerReuseEnabled();

} // namespace D3D12
