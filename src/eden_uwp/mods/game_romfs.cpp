// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/game_romfs.h"

#include <algorithm>
#include <memory>

#include <winrt/base.h>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "eden_uwp/atomic_file.h"
#include "eden_uwp/uwp_file_manager.h"
#include "eden_uwp/uwp_rom_storage.h"

namespace EdenXbox::Mods {
namespace {

std::string Lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}

std::vector<std::string> ReadTop(const std::string& game_path) {
    if (!HasUsableHeaderKey()) return {};
    // As the library reads titles: a system for the loader's file services, never initialized.
    Core::System system;
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(MakeUwpFilesystem());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    const auto file = system.GetFilesystem()->OpenFile(game_path, FileSys::OpenMode::Read);
    if (!file) return {};
    const auto loader = Loader::GetLoader(system, file);
    FileSys::VirtualFile romfs;
    if (!loader || loader->ReadRomFS(romfs) != Loader::ResultStatus::Success || !romfs) return {};
    const auto root = FileSys::ExtractRomFS(romfs);
    if (!root) return {};
    std::vector<std::string> top;
    for (const auto& folder : root->GetSubdirectories()) top.push_back(Lowered(folder->GetName()));
    return top;
}

} // namespace

std::vector<std::string> GameRomFsTop(const std::string& game_path, const std::filesystem::path& cache) {
    if (const auto kept = ReadWholeFile(cache)) {
        std::vector<std::string> top;
        std::size_t start = 0;
        while (start < kept->size()) {
            const auto end = std::min(kept->find('\n', start), kept->size());
            if (end > start) top.push_back(kept->substr(start, end - start));
            start = end + 1;
        }
        return top;
    }
    std::vector<std::string> top;
    try {
        top = ReadTop(game_path);
    } catch (const std::exception& error) {
        LOG_WARNING(Frontend, "Mods: cannot read the game's RomFS: {}", error.what());
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Frontend, "Mods: cannot read the game's RomFS: {:08X}", static_cast<std::uint32_t>(error.code()));
    }
    LOG_INFO(Frontend, "Mods: the game's RomFS has {} top folders", top.size());
    if (!top.empty()) {
        std::string text;
        for (const auto& name : top) text += name + '\n';
        std::error_code ec;
        std::filesystem::create_directories(cache.parent_path(), ec);
        WriteFileAtomically(cache, text);
    }
    return top;
}

} // namespace EdenXbox::Mods
