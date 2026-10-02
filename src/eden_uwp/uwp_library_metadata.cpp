// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_library_metadata.h"
#include <winrt/base.h>
#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"

namespace EdenXbox {
void ReadLibraryMetadata(const std::filesystem::path& root,
                         std::vector<LibraryEntry>& entries, std::stop_token stop) {
    // Core construction provides the loader's filesystem services. Initialize
    // would reserve guest DRAM and start timing; metadata does not need either.
    Core::System system;
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    for (auto& entry : entries) {
        if (stop.stop_requested()) break;
        entry.metadata_loaded = true;
        try {
            const auto path = (root / entry.relative_path).u8string();
            const auto file = system.GetFilesystem()->OpenFile(
                std::string{reinterpret_cast<const char*>(path.data()), path.size()}, FileSys::OpenMode::Read);
            auto loader = Loader::GetLoader(system, file);
            if (!loader) continue;
            std::string title;
            if (loader->ReadTitle(title) == Loader::ResultStatus::Success && !title.empty())
                entry.name = winrt::to_hstring(title).c_str();
            FileSys::NACP control;
            if (loader->ReadControlData(control) == Loader::ResultStatus::Success)
                entry.developer = winrt::to_hstring(control.GetDeveloperName()).c_str();
            std::vector<u8> icon;
            if (loader->ReadIcon(icon) == Loader::ResultStatus::Success && icon.size() <= 1024 * 1024)
                entry.icon = std::move(icon);
            LOG_INFO(Frontend, "Library metadata: title '{}', icon {} bytes", title, entry.icon.size());
        } catch (const std::exception&) {
            // Missing keys/control data or an unreadable dump keeps filename
            // and placeholder; one entry must not take down the whole library.
        } catch (const winrt::hresult_error&) {}
    }
}
}
