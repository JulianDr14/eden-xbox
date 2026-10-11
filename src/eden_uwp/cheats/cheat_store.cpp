// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/cheats/cheat_store.h"

#include <charconv>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/memory/dmnt_cheat_vm.h"
#include "eden_uwp/atomic_file.h"
#include "eden_uwp/mods/mod_settings.h"

namespace EdenXbox::Cheats {
namespace {
static_assert(WordBudget == Core::Memory::DmntCheatVm::MaximumProgramOpcodeCount);

std::filesystem::path FromUtf8(std::string_view text) {
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

std::string ToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// Writes `bytes` unless the file already holds them, so unchanged data costs no write.
bool WriteIfChanged(const std::filesystem::path& path, std::string_view bytes) {
    if (const auto current = ReadWholeFile(path); current && *current == bytes) return true;
    return WriteFileAtomically(path, bytes);
}
} // namespace

std::string HexId(std::uint64_t title_id) {
    return fmt::format("{:016X}", title_id);
}

std::string HexId(const BuildId& build_id) {
    std::string text;
    for (std::size_t i = 0; i < 8; ++i) text += fmt::format("{:02X}", build_id[i]);
    return text;
}

std::filesystem::path TitleLoadDirectory(std::uint64_t title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / HexId(title_id);
}

CheatCatalog LoadCatalog(const std::filesystem::path& title_dir, const BuildId& build_id) {
    FileSys::RealVfsFilesystem filesystem;
    const auto dir = filesystem.OpenDirectory(ToUtf8(title_dir), FileSys::OpenMode::Read);
    // Cheats of mods the player turned off do not load (Mods::ApplyModChoices), so they are not
    // listed either. The folder is named after the title id.
    const auto folder = title_dir.filename().string();
    std::uint64_t title_id = 0;
    std::from_chars(folder.data(), folder.data() + folder.size(), title_id, 16);
    const auto disabled = Mods::DisabledFolders(Mods::DefaultModPaths(), title_id);
    auto files = FileSys::ReadCheatFiles(dir, build_id, disabled);
    std::vector<CheatSet> sets;
    sets.reserve(files.size());
    for (auto& file : files) {
        const auto category = CategoryOfMod(file.mod);
        sets.push_back({std::move(file.mod), category, std::move(file.cheats),
                        FromUtf8(file.toggles_dir->GetFullPath()) / FromUtf8(file.toggles_name)});
    }
    return CheatCatalog{std::move(sets)};
}

bool InstallDbCheats(const std::filesystem::path& title_dir, CheatCategory category,
                     const BuildId& build_id, std::string_view cheats) {
    const auto folder = title_dir / FromUtf8(ModOfCategory(category)) / "cheats";
    const auto toggles_path = folder / "toggles.txt";
    const std::string existing = ReadWholeFile(toggles_path).value_or(std::string{});
    auto entries = Core::Memory::TextCheatParser{}.Parse(cheats);
    for (auto& entry : entries) entry.enabled = false;
    Core::Memory::ApplyCheatToggles(entries, Core::Memory::ParseCheatToggles(existing));
    // Toggles first: a cheats file without them would run every cheat at the next boot.
    return WriteIfChanged(toggles_path, MergeCheatToggles(existing, entries)) &&
           WriteIfChanged(folder / (HexId(build_id) + ".txt"), cheats);
}

bool SaveToggles(const CheatSet& set) {
    const std::string existing = ReadWholeFile(set.toggles).value_or(std::string{});
    return WriteIfChanged(set.toggles, MergeCheatToggles(existing, set.cheats));
}

} // namespace EdenXbox::Cheats
