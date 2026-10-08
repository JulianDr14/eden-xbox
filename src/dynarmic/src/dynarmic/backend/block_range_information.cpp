// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <algorithm>

#include <boost/icl/interval_set.hpp>
#include "common/common_types.h"
#include "common/container/unordered_set.h"

namespace Dynarmic::Backend {

template<typename P>
void BlockRangeInformation<P>::AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location) {
    if (boost::icl::is_empty(range)) {
        return;
    }
    const P first = boost::icl::first(range);
    const P last = boost::icl::last(range);
    for (P page = first >> PAGE_BITS;; ++page) {
        pages[page].push_back(Entry{first, last, location});
        if (page == (last >> PAGE_BITS)) {
            break;
        }
    }
}

template<typename P>
void BlockRangeInformation<P>::ClearCache() {
    pages.clear();
}

template<typename P>
void BlockRangeInformation<P>::InvalidatePage(P page, P first, P last, ::Common::unordered_set<IR::LocationDescriptor>& erase_locations) {
    const auto it = pages.find(page);
    if (it == pages.end()) {
        return;
    }
    std::vector<Entry>& entries = it->second;
    std::erase_if(entries, [&](const Entry& entry) {
        if (entry.last < first || entry.first > last) {
            return false;
        }
        erase_locations.insert(entry.location);
        return true;
    });
    if (entries.empty()) {
        pages.erase(it);
    }
}

template<typename P>
::Common::unordered_set<IR::LocationDescriptor> BlockRangeInformation<P>::InvalidateRanges(const boost::icl::interval_set<P>& ranges) {
    ::Common::unordered_set<IR::LocationDescriptor> erase_locations;
    for (const auto& interval : ranges) {
        if (boost::icl::is_empty(interval)) {
            continue;
        }
        const P first = boost::icl::first(interval);
        const P last = boost::icl::last(interval);
        const P first_page = first >> PAGE_BITS;
        const P last_page = last >> PAGE_BITS;
        if (static_cast<u64>(last_page - first_page) < pages.size()) {
            for (P page = first_page;; ++page) {
                InvalidatePage(page, first, last, erase_locations);
                if (page == last_page) {
                    break;
                }
            }
            continue;
        }
        // A range wider than the pages known (a whole module unmapped): walk the pages instead.
        std::vector<P> touched;
        for (const auto& [page, entries] : pages) {
            if (page >= first_page && page <= last_page) {
                touched.push_back(page);
            }
        }
        for (const P page : touched) {
            InvalidatePage(page, first, last, erase_locations);
        }
    }
    // A block crossing a page is in more than one bucket: the copies in pages outside the
    // invalidated ranges stay, and a later invalidation only names it again.
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
