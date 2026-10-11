// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How the files of an archive become mods. Mod authors pack the same thing many ways: a folder
// with romfs/exefs/cheats, the Atmosphere layout (atmosphere/contents/<title id>/...), Atmosphere's
// exefs_patches/<name>/<build id>.ips, loose .ips or .pchtxt files, the game's own folders with no
// romfs/ around them, or several of these side by side as variants (30 and 60 FPS, one per
// language...). This reads only the list of paths, so it is decided before anything is extracted,
// and the player picks among variants.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace EdenXbox::Mods {

/// A file of an archive: its path inside it ('/'-separated) and its size.
struct ArchiveItem {
    std::string path;
    std::uint64_t size{};
};

/// One file to place: from its archive path to its path inside the mod folder.
struct LayoutFile {
    std::string from;
    std::string to;
    std::uint64_t size{};
};

/// One mod found in an archive.
struct ModCandidate {
    std::string name;  ///< Suggested name (UTF-8): the folder the author gave it, or the archive's.
    std::string root;  ///< Its folder inside the archive ("" for the top).
    std::vector<LayoutFile> files;
    std::uint8_t parts{}; ///< ModPart bits.
    std::uint64_t bytes{};
    /// It sits under atmosphere/contents/<id> for another title id than the game's (an update's
    /// or DLC's id, or another game): it may still work, but the player is told.
    bool other_title{};
};

/// The mods of an archive. Empty when nothing in it is a mod.
struct LayoutPlan {
    std::vector<ModCandidate> candidates;
};

/// Reads the mods out of `items`. `title_id` is the game's (16 upper-case hex digits), used to
/// flag Atmosphere folders of other titles; `archive_name` names mods the archive leaves unnamed.
/// `romfs_top` (the game's top RomFS folders, lower-cased) finds mods that ship the game's folders
/// with no romfs/ around them; without it their start is guessed from the archive's shape.
LayoutPlan PlanLayout(const std::vector<ArchiveItem>& items, std::string_view title_id,
                      std::string_view archive_name, const std::vector<std::string>& romfs_top = {});

/// A folder name under load/<title id>/ for `name`: no characters Windows refuses, no reserved
/// device names, at most 64 characters, and none of `taken` (case-insensitive), adding " (2)"...
std::string FolderName(std::string_view name, const std::vector<std::string>& taken);

/// True when `path` (an archive path) is safe to write under a folder: relative, no "." or ".."
/// steps, no drive, no alternate data stream, no empty step.
bool SafeRelativePath(std::string_view path);

} // namespace EdenXbox::Mods
