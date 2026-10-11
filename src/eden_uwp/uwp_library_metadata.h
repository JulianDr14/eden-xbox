// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/settings_enums.h"
#include "eden_uwp/game_library.h"
#include "eden_uwp/library_cache.h"
namespace EdenXbox {
struct MetadataOptions {
    Settings::Language language{Settings::Language::EnglishAmerican}; ///< Title language.
    bool icons{};      ///< Keep icons in the entries (visible window); records store them anyway.
    bool cache_only{}; ///< Never open a game: only what the cache already has.
};
// The cache first; on a miss, the same loaders as qt_common/game_list/worker.cpp, and the result
// is stored. A Core::System (filesystem services only, no guest Initialize/Load/Run) is created
// only for the first miss, so a fully cached page costs a few file reads.
void ReadLibraryMetadata(const std::filesystem::path& root, std::vector<LibraryEntry>& entries,
                         const LibraryCache& cache, MetadataOptions options, std::stop_token stop);
}
