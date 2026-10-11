// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_settings.h"

#include <algorithm>
#include <charconv>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "eden_uwp/atomic_file.h"
#include "eden_uwp/mods/mod_manifest.h"

namespace EdenXbox::Mods {

ModPaths DefaultModPaths() {
    return {Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir),
            Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "mods"};
}

void ApplyModChoices(const ModPaths& paths) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::directory_iterator it{paths.data, ec}, end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        u64 title_id = 0;
        if (name.size() != 21 || !name.ends_with(".json") ||
            std::from_chars(name.data(), name.data() + 16, title_id, 16).ec != std::errc{})
            continue;
        const auto text = ReadWholeFile(it->path());
        const auto mods = text ? ParseManifest(*text) : std::nullopt;
        if (!mods) continue;
        auto choices = ToLoaderSettings(*mods);
        if (!choices.disabled.empty())
            LOG_INFO(Frontend, "Mods: {} of {} off for {:016X}", choices.disabled.size(), mods->size(), title_id);
        Settings::values.disabled_addons[title_id] = std::move(choices.disabled);
        Settings::values.mod_priority[title_id] = std::move(choices.priority);
    }
}

std::vector<std::string> DisabledFolders(const ModPaths& paths, std::uint64_t title_id) {
    const auto text = ReadWholeFile(paths.data / (TitleFolder(title_id) + ".json"));
    const auto mods = text ? ParseManifest(*text) : std::nullopt;
    return mods ? ToLoaderSettings(*mods).disabled : std::vector<std::string>{};
}

std::size_t ActiveMods(const ModPaths& paths, std::uint64_t title_id) {
    const auto text = ReadWholeFile(paths.data / (TitleFolder(title_id) + ".json"));
    const auto mods = text ? ParseManifest(*text) : std::nullopt;
    if (!mods) return 0;
    return static_cast<std::size_t>(
        std::count_if(mods->begin(), mods->end(), [](const ModRecord& mod) { return mod.enabled; }));
}

void ClearScratch(const ModPaths& paths) {
    std::error_code ec;
    for (const char* kind : {"staging", "trash"}) std::filesystem::remove_all(paths.data / kind, ec);
}

} // namespace EdenXbox::Mods
