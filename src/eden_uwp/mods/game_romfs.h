// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Many mods ship the game's own folders with no romfs/ around them. Knowing the folders at the top
// of the game's RomFS tells where such a mod's files start, however the author wrapped them.

#include <filesystem>
#include <string>
#include <vector>

namespace EdenXbox::Mods {

/// The folders at the top of the RomFS of the game at `game_path` (a library launch path),
/// lower-cased. Read once and kept in `cache`; empty when the game or its keys cannot be read.
std::vector<std::string> GameRomFsTop(const std::string& game_path, const std::filesystem::path& cache);

} // namespace EdenXbox::Mods
