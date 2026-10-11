// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The CNX index (nx-links): a JSON file of fan translations and modifications, each one archive
// laid out as <title id>/romfs/... with its download link. The store lists the entries of the game
// that is open; the file is cached with its ETag like the cheats database, so it is downloaded
// only when it changes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace EdenXbox::Mods::Cnx {

inline constexpr std::string_view IndexUrl =
    "https://raw.githubusercontent.com/gamemoddesignbr/nx-links/master/nx-links-v204.json";

enum class Kind : std::uint8_t { Translation, Modification };

struct Entry {
    std::string name;
    Kind kind{};
    std::string url;
    std::string size; ///< As the index writes it ("53.7 MB").
};

/// The enabled entries for `title_id` (16 hex digits, any case). The title id of an entry is the
/// name of the archive its link points to, or the first folder of the file it installs.
std::vector<Entry> ParseIndex(std::string_view json, std::string_view title_id);

/// "53.7 MB" as bytes (0 when it cannot be read): enough to check free space before a download.
std::uint64_t ParseSize(std::string_view text);

} // namespace EdenXbox::Mods::Cnx
