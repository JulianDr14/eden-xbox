// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The cheats on disk, where Eden's loader reads them: `load/<title id>/<mod>/cheats/<build id>.txt`
// with Atmosphere's `toggles.txt` next to it. The online database's cheats get mod folders of
// their own (one per category), written whole: the toggles say which are on, so nothing is
// copied twice and the player's own cheat folders are never touched but for their toggles.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "eden_uwp/cheats/cheat_catalog.h"

namespace EdenXbox::Cheats {

/// The main module's build id; cheat files are named after its first 8 bytes.
using BuildId = std::array<std::uint8_t, 0x20>;

/// 16 upper-case hex digits, as Eden names title folders and cheat files.
std::string HexId(std::uint64_t title_id);
std::string HexId(const BuildId& build_id);

/// `load/<title id>` in Eden's user folder.
std::filesystem::path TitleLoadDirectory(std::uint64_t title_id);

/// Every cheats file the loader reads for `build_id`, as a catalog.
CheatCatalog LoadCatalog(const std::filesystem::path& title_dir, const BuildId& build_id);

/// Writes the database's cheats file for `category` and `build_id`. Cheats new to it start off
/// (toggles.txt names every cheat, since Eden turns on the ones it does not name); the others keep
/// their toggles. False when the files could not be written.
bool InstallDbCheats(const std::filesystem::path& title_dir, CheatCategory category,
                     const BuildId& build_id, std::string_view cheats);

/// Saves `set`'s toggles, keeping those of the game's other builds.
bool SaveToggles(const CheatSet& set);

} // namespace EdenXbox::Cheats
