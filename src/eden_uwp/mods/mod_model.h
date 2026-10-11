// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mod manager's vocabulary. A mod is a folder under load/<title id>/ that the loader layers
// over the game (core/file_sys/patch_manager.cpp); the manager keeps, per game, which folders it
// knows, where each came from, whether it is on and the order they apply in (a manifest, see
// mod_manifest.h). What a folder actually changes is read from disk, never stored, so a manifest
// can not drift from the files.

#include <cstdint>
#include <string>
#include <vector>

namespace EdenXbox::Mods {

/// What a mod changes. A mod may do several.
enum ModPart : std::uint8_t {
    RomFs = 1 << 0,    ///< Game files replaced (romfs, romfslite).
    RomFsExt = 1 << 1, ///< Game files deleted (.stub) or patched (.ips) in place (romfs_ext).
    ExeFs = 1 << 2,    ///< Executables replaced (exefs).
    Patch = 1 << 3,    ///< Code patches for one game version (exefs/*.ips, *.pchtxt).
    Cheats = 1 << 4,   ///< Cheat lists (cheats/<build id>.txt).
};

enum class SourceKind : std::uint8_t {
    Local,      ///< A folder that was already there (copied by hand, or an older install).
    Archive,    ///< Installed from a .zip, .7z or .rar.
    GameBanana, ///< Downloaded from GameBanana.
    Cnx,        ///< Downloaded from the CNX index.
};

/// Where a mod came from: enough to tell its updates apart and to show its origin.
struct ModSource {
    SourceKind kind{SourceKind::Local};
    std::string archive;      ///< File name it was installed from.
    std::int64_t mod_id{};    ///< Online id (GameBanana mod) when downloaded.
    std::int64_t file_id{};   ///< Online file id the archive was.
    std::string version;      ///< Version text the source gave.
    std::int64_t updated{};   ///< Unix time of the file installed, as the source dates it.
    std::string url;          ///< Where it was downloaded from (the CNX index names files by link).

    bool operator==(const ModSource&) const = default;
};

/// One mod of a game, as the manifest keeps it.
struct ModRecord {
    std::string folder;  ///< Folder name under load/<title id>/: what the loader matches.
    std::string name;    ///< Name shown (UTF-8).
    ModSource source;
    bool enabled{true};
    std::int64_t installed{}; ///< Unix time.

    bool operator==(const ModRecord&) const = default;
};

/// One file of an installed mod.
struct ModFileEntry {
    std::string path; ///< Relative to the mod's folder ('/'-separated, as written).
    std::uint64_t bytes{};
};

/// What an installed folder holds, read from disk.
struct ModContents {
    std::uint8_t parts{};             ///< ModPart bits.
    std::vector<std::string> build_ids; ///< Game versions its patches and cheats target (16 hex).
    std::vector<std::string> files;   ///< Game paths it replaces ("romfs/...", "exefs/main"), lower case.
    std::uint64_t bytes{};
    std::vector<ModFileEntry> listing; ///< Every file of the folder, sorted by path.
};

/// Whether a mod's code patches or cheats fit the version of the game that is installed.
enum class Compat : std::uint8_t {
    Any,          ///< It has nothing version-bound (only files).
    Matches,      ///< It has patches or cheats for this version.
    OtherVersion, ///< All its patches and cheats are for other versions: they will not apply.
};

} // namespace EdenXbox::Mods
