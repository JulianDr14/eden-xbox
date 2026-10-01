// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace D3D12::HeapPacking {
struct Placement {
    std::uint64_t offset;
    std::uint64_t waste;
};

/// The smallest fitting range preserves larger holes. Padding remains a reusable free range.
inline std::optional<Placement> Fit(std::uint64_t offset, std::uint64_t bytes,
                                   std::uint64_t size, std::uint64_t alignment) {
    if (!size || !alignment || (alignment & (alignment - 1))) return std::nullopt;
    const auto padding = (alignment - (offset & (alignment - 1))) & (alignment - 1);
    if (padding > bytes || size > bytes - padding ||
        offset > std::numeric_limits<std::uint64_t>::max() - padding - size)
        return std::nullopt;
    return Placement{offset + padding, bytes - size};
}

/// Heaps require alignment, not power-of-two capacity. Zero reports overflow/invalid input.
inline std::uint64_t BlockSize(std::uint64_t size, std::uint64_t alignment,
                               std::uint64_t minimum) {
    alignment = std::max<std::uint64_t>(alignment, 65536);
    if (alignment & (alignment - 1)) return 0;
    size = std::max(size, minimum);
    const auto padding = (alignment - (size & (alignment - 1))) & (alignment - 1);
    if (size > std::numeric_limits<std::uint64_t>::max() - padding) return 0;
    return size + padding;
}
} // namespace D3D12::HeapPacking
