// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What an installed mod changes, read from its folder the way the loader will read it
// (core/file_sys/patch_manager.cpp): its parts, the game versions its patches and cheats target,
// and the game files it replaces, which is what two mods can fight over.

#include <filesystem>
#include <string>
#include <string_view>

#include "eden_uwp/mods/mod_model.h"

namespace EdenXbox::Mods {

/// Reads `folder` (load/<title id>/<mod>). An unreadable folder reads as empty.
ModContents InspectFolder(const std::filesystem::path& folder);

/// The build id a .pchtxt targets (its "@nsobid-" line), as 16 upper-case hex digits; empty when
/// the text names none.
std::string PchtxtBuildId(std::string_view text);

/// A build id written any way (any case, any length, padded with zeros) as the 16 upper-case hex
/// digits cheats are named with; empty when `text` is not hex.
std::string ShortBuildId(std::string_view text);

} // namespace EdenXbox::Mods
