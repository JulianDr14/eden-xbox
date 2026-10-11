// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Hands the player's mod choices to the loader before a game boots: for every game with a mod
// manifest, which folders are off (Settings::values.disabled_addons) and the order they apply in
// (Settings::values.mod_priority). Reading every manifest needs no title id from the boot path, and
// there is one small file per game with mods.

#include "eden_uwp/mods/mod_installer.h"

namespace EdenXbox::Mods {

/// load/ and mods/ under the emulator's folder.
ModPaths DefaultModPaths();

/// Fills the loader's settings from the manifests under `paths.data`.
void ApplyModChoices(const ModPaths& paths);

/// The mod folders of one game that are off, for pages that read mod files themselves (cheats).
std::vector<std::string> DisabledFolders(const ModPaths& paths, std::uint64_t title_id);

/// How many of one game's mods are on, as its manifest says (the in-game menu shows it).
std::size_t ActiveMods(const ModPaths& paths, std::uint64_t title_id);

/// Deletes what an install or uninstall cut short left in staging and trash, for every game.
void ClearScratch(const ModPaths& paths);

} // namespace EdenXbox::Mods
