// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Downloads one title file of the cheats database, keeping a copy and its ETag in LocalCache:
// asking again sends If-None-Match, so an unchanged file costs a 304 with no body.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "eden_uwp/cheats/cheat_catalog.h"

namespace EdenXbox::Cheats {

enum class FetchStatus : std::uint8_t {
    Fresh,    ///< The server's current file (downloaded or confirmed unchanged).
    Missing,  ///< The database has no file for the title.
    Stale,    ///< The server could not be reached; the cached copy is used.
    Offline,  ///< The server could not be reached and there is no copy.
};

struct FetchResult {
    FetchStatus status{FetchStatus::Offline};
    std::string json; ///< Empty unless Fresh or Stale.
};

/// LocalCache\cheats in the app's data.
std::filesystem::path DefaultCheatCacheDirectory();

/// The database's `category` file for `title_id` (16 hex digits). Blocks for at most ~20 s; call
/// it from a worker with a multithreaded apartment.
FetchResult FetchCheatDb(const std::filesystem::path& cache_dir, CheatCategory category,
                         const std::string& title_id);

} // namespace EdenXbox::Cheats
