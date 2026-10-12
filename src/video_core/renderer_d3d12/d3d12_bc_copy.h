// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/common_types.h"
#include <algorithm>
#include <optional>
#include <vector>

namespace D3D12 {

struct BcCopyAxis {
    u32 source_end;
    u32 blocks;
};

/// Alias extents count storage blocks. Convert to logical source texels, allowing only the
/// final block to extend beyond the logical mip (1x1/2x2 tails still occupy a whole block).
constexpr std::optional<u32> AliasBlockExtent(u32 blocks, s32 offset, u32 size, u32 block) {
    if (!blocks || offset < 0 || !block || static_cast<u32>(offset) >= size ||
        static_cast<u32>(offset) % block != 0) return std::nullopt;
    const u32 remaining = size - static_cast<u32>(offset);
    if (blocks > remaining / block + (remaining % block != 0)) return std::nullopt;
    return static_cast<u32>(std::min<u64>(u64{blocks} * block, remaining));
}

/// GPU-generated arrays may have only some blocks initialized. Track exact coverage, one bit per
/// compressed block, so an unrelated mip/layer/rectangle never reads an uninitialized shadow.
class BcBlockCoverage {
  public:
    void Reset(u32 width, u32 height, u32 levels, u32 layers) {
        subresources.clear();
        u64 count = 0;
        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 level = 0; level < levels; ++level) {
                const u32 w = (std::max(1U, width >> level) + 3) / 4;
                const u32 h = (std::max(1U, height >> level) + 3) / 4;
                subresources.push_back({count, w, h});
                count += u64{w} * h;
            }
        }
        valid.assign(static_cast<size_t>((count + 63) / 64), 0);
    }
    void Clear() { std::fill(valid.begin(), valid.end(), 0); }
    void Fill() { std::fill(valid.begin(), valid.end(), ~u64{0}); }
    bool Contains(u32 subresource, u32 x, u32 y, u32 width, u32 height) const {
        if (!InBounds(subresource, x, y, width, height))
            return false;
        const auto &sub = subresources[subresource];
        for (u32 row = 0; row < height; ++row)
            for (u32 col = 0; col < width; ++col) {
                const u64 bit = sub.offset + u64{y + row} * sub.width + x + col;
                if ((valid[bit / 64] & (u64{1} << (bit % 64))) == 0)
                    return false;
            }
        return true;
    }
    void Include(u32 subresource, u32 x, u32 y, u32 width, u32 height) {
        if (!InBounds(subresource, x, y, width, height))
            return;
        const auto &sub = subresources[subresource];
        for (u32 row = 0; row < height; ++row)
            for (u32 col = 0; col < width; ++col) {
                const u64 bit = sub.offset + u64{y + row} * sub.width + x + col;
                valid[bit / 64] |= u64{1} << (bit % 64);
            }
    }

  private:
    bool InBounds(u32 index, u32 x, u32 y, u32 w, u32 h) const {
        if (index >= subresources.size())
            return false;
        const auto &sub = subresources[index];
        return w && h && x < sub.width && y < sub.height && w <= sub.width - x &&
               h <= sub.height - y;
    }
    struct Subresource {
        u64 offset;
        u32 width, height;
    };
    std::vector<Subresource> subresources;
    std::vector<u64> valid;
};

/// Source coordinates are BC texels; destination coordinates are one integer texel per block.
/// A partial last block is legal only at the mip edge. Subtraction avoids overflow on bad input.
constexpr std::optional<BcCopyAxis> PlanBcCopyAxis(s32 source_offset, u32 extent, u32 source_size,
                                                   s32 destination_offset, u32 destination_size,
                                                   u32 block_size = 4,
                                                   const char** failure = nullptr) {
    const auto reject = [failure](const char* reason) -> std::optional<BcCopyAxis> {
        if (failure) *failure = reason;
        return std::nullopt;
    };
    if (failure) *failure = nullptr;
    if (source_offset < 0) return reject("negative source offset");
    if (destination_offset < 0) return reject("negative destination offset");
    if (extent == 0) return reject("zero extent");
    if (block_size == 0) return reject("zero block size");
    const auto src = static_cast<u32>(source_offset);
    const auto dst = static_cast<u32>(destination_offset);
    if (src >= source_size) return reject("source offset outside mip");
    if (dst >= destination_size) return reject("destination offset outside mip");
    if (src % block_size != 0) return reject("source offset not block aligned");
    if (extent > source_size - src) return reject("extent exceeds source mip");
    if (extent % block_size != 0 && extent != source_size - src)
        return reject("partial block does not end at source mip edge");
    const u32 blocks = extent / block_size + (extent % block_size != 0);
    if (blocks > destination_size - dst) {
        return reject("block count exceeds destination mip");
    }
    return BcCopyAxis{src + extent, blocks};
}

} // namespace D3D12
