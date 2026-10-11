// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The order mod folders apply in. A frontend may name the folders of a title by priority
// (Settings::values.mod_priority, most important first); folders the list does not name follow,
// by name, so with no list this is the plain name order mods always had. LayeredFS and LayeredExeFS
// let the first layer win a file, so they take this order as is; IPS patches are applied one over
// the other and the last one wins, so they take it reversed.

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace FileSys {

/// Sorts `items` (mod folders) by `priority`, then by name. `name` maps an item to its folder name.
/// Null items are dropped: a title may have no SD card or no load root.
template <typename T, typename Name>
void SortByModPriority(std::vector<T>& items, const std::vector<std::string>& priority, Name name) {
    std::erase(items, nullptr);
    // Each key is computed once: folder names come from the file system.
    std::vector<std::pair<std::pair<std::size_t, std::string>, T>> keyed;
    keyed.reserve(items.size());
    for (auto& item : items) {
        std::string item_name = name(item);
        const auto rank = static_cast<std::size_t>(
            std::find(priority.begin(), priority.end(), item_name) - priority.begin());
        keyed.push_back({{rank, std::move(item_name)}, std::move(item)});
    }
    std::sort(keyed.begin(), keyed.end(),
              [](const auto& l, const auto& r) { return l.first < r.first; });
    items.clear();
    for (auto& entry : keyed) {
        items.push_back(std::move(entry.second));
    }
}

} // namespace FileSys
