// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include "common/common_types.h"
namespace VideoCommon {
/// Optional backend policy. A time budget stops starting more work; one download may exceed it.
struct TextureGcPolicy {
    bool high_priority{};
    bool aggressive{};
    u64 min_age{};
    std::size_t iterations{};
    u64 time_budget_us{}; // zero = emergency/unbounded; preserves allocation recovery
    std::size_t max_downloads{};
};
}
