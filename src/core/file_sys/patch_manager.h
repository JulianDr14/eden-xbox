// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/memory/dmnt_cheat_types.h"

namespace Core {
class System;
}

namespace Service::FileSystem {
class FileSystemController;
}

namespace FileSys {

class ContentProvider;
class NCA;
class NACP;

enum class PatchType { Update, DLC, Mod };

enum class PatchSource {
    Unknown,
    NAND,
    SDMC,
    External,
    Packed,
};

struct Patch {
    bool enabled;
    std::string name;
    std::string version;
    PatchType type;
    u64 program_id;
    u64 title_id;
    PatchSource source;
    std::string location;
    u32 numeric_version{0};
};

/// One cheats file a game loads, with its toggles applied: `<mod>/cheats/<build id>.txt` (toggled
/// by `toggles.txt` next to it, as Atmosphere does) or a loose `cheat_*.txt` in the title's load
/// folder (toggled by `<name>.toggles`).
struct CheatFile {
    std::string mod;                              ///< Mod folder, or the loose file's name.
    std::vector<Core::Memory::CheatEntry> cheats; ///< As TextCheatParser returns them.
    VirtualDir toggles_dir;                       ///< Where the toggles file is (or would be).
    std::string toggles_name;
};

// A centralized class to manage patches to games.
class PatchManager {
public:
    using BuildID = std::array<u8, 0x20>;
    using Metadata = std::pair<std::unique_ptr<NACP>, VirtualFile>;

    explicit PatchManager(u64 title_id_,
                          const Service::FileSystem::FileSystemController& fs_controller_,
                          const ContentProvider& content_provider_);
    ~PatchManager();

    [[nodiscard]] u64 GetTitleID() const;

    // Currently tracked ExeFS patches:
    // - Game Updates
    [[nodiscard]] VirtualDir PatchExeFS(VirtualDir exefs) const;

    // Currently tracked NSO patches:
    // - IPS
    // - IPSwitch
    [[nodiscard]] std::vector<u8> PatchNSO(const std::vector<u8>& nso,
                                           const std::string& name) const;

    // Checks to see if PatchNSO() will have any effect given the NSO's build ID.
    // Used to prevent expensive copies in NSO loader.
    [[nodiscard]] bool HasNSOPatch(const BuildID& build_id, std::string_view name) const;

    // Creates a CheatList object with all
    [[nodiscard]] std::vector<Core::Memory::CheatEntry> CreateCheatList(
        const BuildID& build_id) const;

    // Currently tracked RomFS patches:
    // - Game Updates
    // - LayeredFS
    [[nodiscard]] VirtualFile PatchRomFS(const NCA* base_nca, VirtualFile base_romfs,
                                         ContentRecordType type = ContentRecordType::Program,
                                         VirtualFile packed_update_raw = nullptr,
                                         bool apply_layeredfs = true) const;

    // Returns a vector of patches
    [[nodiscard]] std::vector<Patch> GetPatches(VirtualFile update_raw = nullptr) const;

    // If the game update exists, returns the u32 version field in its Meta-type NCA. If that fails,
    // it will fallback to the Meta-type NCA of the base game. If that fails, the result will be
    // std::nullopt
    [[nodiscard]] std::optional<u32> GetGameVersion() const;

    // Given title_id of the program, attempts to get the control data of the update and parse
    // it, falling back to the base control data.
    [[nodiscard]] Metadata GetControlMetadata() const;

    // Version of GetControlMetadata that takes an arbitrary NCA
    [[nodiscard]] Metadata ParseControlNCA(const NCA& nca) const;

    /// @brief Gets NACP metadata (accounting for any patches or updates)
    [[nodiscard]] static PatchManager::Metadata GetMetadataFromBaseOrUpdate(Core::System& system, u64 application_id) noexcept;

private:
    [[nodiscard]] VirtualDir GetModificationLoadRoot(bool sdmc = false) const;
    [[nodiscard]] std::vector<std::string> GetDisabledAddons() const;
    /// Mod folders in the order they apply (mod_priority.h); drops null roots.
    void SortModDirectories(std::vector<VirtualDir>& dirs) const;
    void ApplyLayeredFS(VirtualFile& romfs, ContentRecordType type) const;
    [[nodiscard]] std::vector<VirtualFile> CollectPatches(const std::vector<VirtualDir>& patch_dirs,
                                                          const std::string& build_id) const;

    u64 title_id;
    std::optional<u64> parent_title_id;
    const Service::FileSystem::FileSystemController& fs_controller;
    const ContentProvider& content_provider;
};

/// The cheats files under one title's load folder (`load/<title id>`), in the order the loader
/// joins them into the cheat list; mods named in `disabled` are left out.
[[nodiscard]] std::vector<CheatFile> ReadCheatFiles(const VirtualDir& title_load_dir,
                                                    const PatchManager::BuildID& build_id,
                                                    std::span<const std::string> disabled);

} // namespace FileSys
