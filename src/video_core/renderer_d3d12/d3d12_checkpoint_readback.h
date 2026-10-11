// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include <span>
#include "video_core/renderer_d3d12/d3d12_gc_readback.h"

namespace D3D12 {

// Copy a fenced footprint into independent tight storage. Validate before touching output;
// malformed regions cannot overflow either span. Source/destination must not overlap.
inline bool CopyCheckpointRows(std::span<const u8> source, std::span<u8> destination,
                               const GcReadbackRegion& region) {
    const u64 count = u64{region.rows} * region.depth;
    if (!count || !region.row_bytes || region.row_pitch < region.row_bytes ||
        region.source_offset > source.size() || region.tight_offset > destination.size() ||
        count > (source.size() - region.source_offset) / region.row_pitch ||
        count > (destination.size() - region.tight_offset) / region.row_bytes) {
        return false;
    }
    for (u64 row = 0; row < count; ++row) {
        std::memcpy(destination.data() + region.tight_offset + row * region.row_bytes,
                    source.data() + region.source_offset + row * region.row_pitch,
                    region.row_bytes);
    }
    return true;
}
} // namespace D3D12
