// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <vector>

#include <boost/icl/discrete_interval.hpp>
#include <boost/icl/interval_set.hpp>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

/// The guest address range of every compiled block, to find the blocks a code write invalidates.
/// Bucketed by guest page: one 16-24 byte entry per block (more only for a block that crosses a
/// page). An interval_map of sets cost a tree node and a set per block, ~385 bytes, 285 MiB for
/// the 777k blocks of a large game.
template<typename P>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location);
    void ClearCache();
    /// The blocks overlapping `ranges`, which are forgotten here.
    ::Common::unordered_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<P>& ranges);

private:
    static constexpr unsigned PAGE_BITS = 12;
    struct Entry {
        P first;
        P last;  ///< Inclusive.
        IR::LocationDescriptor location;
    };
    void InvalidatePage(P page, P first, P last, ::Common::unordered_set<IR::LocationDescriptor>& erase_locations);
    ::Common::unordered_map<P, std::vector<Entry>> pages;
};

}  // namespace Dynarmic::Backend
