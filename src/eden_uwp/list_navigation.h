// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>

namespace EdenXbox {
// Keep selection visible after navigation, asynchronous arrivals or removal.
// State stays owned by the page; drawing and hit tests use the same first row.
inline void ClampListWindow(std::size_t& selected, std::size_t& first,
                            std::size_t count, std::size_t visible) {
    selected = count ? std::min(selected, count - 1) : 0;
    if (selected < first) first = selected;
    if (visible && selected - first >= visible) first = selected - visible + 1;
    first = std::min(first, count > visible ? count - visible : 0);
}
}
