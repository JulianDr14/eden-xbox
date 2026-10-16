// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <utility>
#include "common/common_types.h"
namespace D3D12 {
// Keep full keys stationary. Promoting a hit swaps two indices rather than two large keys.
// Equality remains exact, including the key's variable prefix and D3D12-only tail.
template <typename Key>
[[nodiscard]] size_t FindPipelineTransition(std::span<const Key> keys, std::span<u32> order,
                                           const Key& current) noexcept {
    for (size_t i = 0; i < order.size(); ++i) {
        const u32 index = order[i];
        if (keys[index] != current) continue;
        if (i != 0) std::swap(order[i], order[i - 1]);
        return index;
    }
    return keys.size();
}
} // namespace D3D12
