// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include "eden_uwp/content/content_inspector.h"
#include "eden_uwp/content/content_store.h"
#include "core/file_sys/registered_cache.h"

namespace Core { class System; }
namespace EdenXbox::Content {
ContentStore DefaultStore();
std::uint32_t SupportedSystemVersion();
std::uint32_t ReportedSystemVersion(Core::System& system);
std::uint32_t PackageBaseVersion(FileSys::VfsFilesystem& vfs, const std::string& path, std::uint64_t application);
std::string SourcePath(const ContentStore& store, const Revision& revision);
Error ExecuteImport(ContentStore& store, Catalog& catalog, FileSys::VfsFilesystem& vfs,
                    InspectedContent& content, bool copy_internal, bool have_base,
                    std::stop_token stop = {}, Progress progress = {}, std::uint32_t base_version = 0);
struct LaunchResolution {
    Error error{};
    std::uint64_t application{};
    std::uint32_t version{};
    std::string display_version;
    bool managed{};
    std::array<std::uint8_t, 8> build_id{};
    std::vector<Revision> unavailable;
};
// Populate the same provider used by PatchManager/AOC. Never change persistent choices here.
LaunchResolution ResolveLaunchContent(FileSys::VfsFilesystem& vfs, const std::string& base_path,
                                     FileSys::ManualContentProvider& provider,
                                     bool omit_unavailable = false, std::stop_token stop = {},
                                     std::uint32_t system_version = SupportedSystemVersion());
// Single-launch authorization from the library. Consumed by RunHeadlessBoot.
void AllowMissingForNextLaunch(std::string path);
bool ConsumeAllowMissing(const std::string& path);
} // namespace EdenXbox::Content
