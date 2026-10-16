// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include "common/common_types.h"

namespace D3D12 {

struct UniformUpload {
    std::span<u8> mapped;
    u64 address;
};

/// Suballocate CBV uploads from a small staging reservation, only within its submission.
/// Contents are always copied by the caller. The staging pool owns memory and fence retirement;
/// no reference is reused after its tick changes. At most 4 KiB unused per submission.
class UniformStream {
public:
    static constexpr size_t BLOCK_SIZE = 4096;
    static constexpr size_t ALIGNMENT = 256;

    template <typename Allocate>
    UniformUpload Request(size_t size, u64 current_tick, Allocate&& allocate) {
        const size_t aligned = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        // Preserve the existing path for large cbufs (including all supported 64 KiB views).
        if (aligned > BLOCK_SIZE) return allocate(aligned);
        if (tick != current_tick || block.mapped.empty() || aligned > block.mapped.size() - used) {
            block = allocate(BLOCK_SIZE);
            tick = current_tick;
            used = 0;
        }
        const UniformUpload result{block.mapped.subspan(used, size), block.address + used};
        used += aligned;
        return result;
    }

private:
    UniformUpload block{};
    u64 tick{};
    size_t used{};
};

} // namespace D3D12
