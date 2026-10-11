// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the mods of a game do together: whether each fits the installed game version, and which
// active mods replace the same game files. The loader layers mods in priority order and the first
// one that has a file wins it (mod_priority.h), so a conflict always has a winner the player can
// change by moving a mod up or down.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "eden_uwp/mods/mod_model.h"

namespace EdenXbox::Mods {

/// `contents` against the game's build id (16 upper-case hex digits; empty when unknown).
inline Compat CheckCompat(const ModContents& contents, std::string_view game_build_id) {
    if (contents.build_ids.empty() || game_build_id.empty()) return Compat::Any;
    const bool match = std::find(contents.build_ids.begin(), contents.build_ids.end(),
                                 game_build_id) != contents.build_ids.end();
    if (match) return Compat::Matches;
    // A mod that also replaces files still does that part on any version.
    return Compat::OtherVersion;
}

/// Two active mods that replace the same game files.
struct Conflict {
    std::size_t other{};  ///< Index of the other mod.
    std::size_t files{};  ///< How many game files both replace.
    bool wins{};          ///< This mod comes first, so its files are the ones the game sees.
};

/// The conflicts of every mod: `contents[i]` belongs to the mod at priority `i` (0 first), and
/// `enabled[i]` says whether it is on. Off mods have none. Both file lists are sorted.
inline std::vector<std::vector<Conflict>> FindConflicts(const std::vector<ModContents>& contents,
                                                        const std::vector<bool>& enabled) {
    std::vector<std::vector<Conflict>> out(contents.size());
    for (std::size_t a = 0; a < contents.size(); ++a) {
        if (!enabled[a]) continue;
        for (std::size_t b = a + 1; b < contents.size(); ++b) {
            if (!enabled[b]) continue;
            const auto& fa = contents[a].files;
            const auto& fb = contents[b].files;
            std::size_t shared = 0;
            for (auto i = fa.begin(), j = fb.begin(); i != fa.end() && j != fb.end();) {
                if (*i < *j) ++i;
                else if (*j < *i) ++j;
                else { ++shared; ++i; ++j; }
            }
            if (shared == 0) continue;
            out[a].push_back({b, shared, true});
            out[b].push_back({a, shared, false});
        }
    }
    return out;
}

} // namespace EdenXbox::Mods
