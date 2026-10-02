// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include <optional>
#include <span>
#include "common/common_types.h"

namespace D3D12 {
struct GcReadbackRegion {
    u64 source_offset{}, tight_offset{}, source_bytes{}, tight_bytes{};
    u32 row_bytes{}, row_pitch{}, rows{}, depth{};
};

/// Ordered full subresources only. Enforce downward compaction and the allocation
/// cap before recording any GPU work; reject overflow and non-tight layouts.
inline std::optional<GcReadbackRegion> PlanGcReadbackRegion(
    u64 source_end, u64 tight_offset, u32 row_bytes, u32 row_pitch, u32 rows, u32 depth,
    u64 capacity) {
    if (!row_bytes || row_pitch < row_bytes || row_pitch % 256 || !rows || !depth ||
        source_end < tight_offset || source_end > capacity || source_end > ~u64{0} - 511) {
        return std::nullopt;
    }
    const u64 offset = (source_end + 511) & ~u64{511};
    const u64 row_count = u64{rows} * depth;
    if (offset > capacity || row_count > (capacity - offset) / row_pitch) return std::nullopt;
    return GcReadbackRegion{offset, tight_offset, row_count * row_pitch,
                            row_count * row_bytes, row_bytes, row_pitch, rows, depth};
}

/// The GPU fence must be complete and all regions ordered and nonoverlapping.
/// memmove handles a row overlapping its original footprint. No scratch allocation.
inline void CompactGcReadback(std::span<u8> memory, const GcReadbackRegion& region) {
    const u64 row_count = u64{region.rows} * region.depth;
    if (region.row_bytes == region.row_pitch) {
        std::memmove(memory.data() + region.tight_offset,
                     memory.data() + region.source_offset, region.tight_bytes);
        return;
    }
    for (u64 row = 0; row < row_count; ++row) {
        std::memmove(memory.data() + region.tight_offset + row * region.row_bytes,
                     memory.data() + region.source_offset + row * region.row_pitch,
                     region.row_bytes);
    }
}
} // namespace D3D12
