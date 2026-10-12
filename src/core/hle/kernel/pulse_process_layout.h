// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "common/common_types.h"

namespace Kernel {

// Random choices made when a process is created from metadata: the code offset, the
// alias/heap/stack/kernel-map region offsets, the process-local region and the entropy that
// svcGetInfo reports. A restored process must reuse them so that every guest pointer and value
// it already read stays valid. Guest values only, never host addresses.
struct ProcessLayoutPin {
    u64 aslr_offset{};
    std::array<u64, 4> region_random{};
    u64 plr_address{};
    std::array<u64, 4> entropy{};
    bool operator==(const ProcessLayoutPin&) const = default;
};

} // namespace Kernel
