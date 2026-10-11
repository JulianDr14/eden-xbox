// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The online cheats database, nx-cheats-db by sthetix (github.com/sthetix/nx-cheats-db), which
// collects the community's Atmosphere cheats and updates daily. It keeps one JSON file per title
// and category, so a game downloads only its own cheats (a few KB) instead of the whole database.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdenXbox::Cheats {

enum class CheatCategory : std::uint8_t;

/// Where the database keeps `category`'s file for `title_id` (16 upper-case hex digits).
std::string CheatDbUrl(CheatCategory category, std::string_view title_id);

struct CheatDbTitle {
    std::string cheats;              ///< The build's cheats file: the database's blocks, joined.
    std::vector<std::string> builds; ///< Builds (16 upper-case hex digits) it has cheats for.
    std::vector<std::string> credits; ///< The authors it credits ("By ..."), without repeats.
};

/// Reads a title file: `{"<build id>": {"[Name]": "<cheat text>", ...}, ..., "attribution":
/// {"<file>": "By ...\n", ...}}`. A build may appear under its 16 and its 64 hex digits; both are
/// merged by cheat name. `build_id` is matched on its first 16 hex digits, in any case. nullopt
/// when the text is not such a file.
std::optional<CheatDbTitle> ParseCheatDbTitle(std::string_view json, std::string_view build_id);

} // namespace EdenXbox::Cheats
