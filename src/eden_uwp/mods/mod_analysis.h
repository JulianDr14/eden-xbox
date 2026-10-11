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

/// The names of the `count` largest files a mod gives the game (its romfs and exefs, not its
/// readme or cheats), largest first: what tells the player what it changes.
inline std::vector<std::string> MainFiles(const std::vector<ModFileEntry>& listing, std::size_t count) {
    const auto lowered = [](std::string_view text) {
        std::string out{text};
        for (auto& c : out) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        return out;
    };
    std::vector<const ModFileEntry*> game;
    for (const auto& file : listing) {
        const auto top = lowered(file.path.substr(0, file.path.find('/')));
        if (top == "romfs" || top == "romfslite" || top == "romfs_ext" || top == "exefs") game.push_back(&file);
    }
    std::stable_sort(game.begin(), game.end(),
                     [](const ModFileEntry* a, const ModFileEntry* b) { return a->bytes > b->bytes; });
    std::vector<std::string> names;
    for (std::size_t i = 0; i < game.size() && names.size() < count; ++i)
        names.push_back(game[i]->path.substr(game[i]->path.rfind('/') + 1));
    return names;
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
