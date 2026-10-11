// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One game's mods as the manager keeps them: a JSON file per game (mods/<title id>.json) listing
// its mod folders in priority order, most important first, with their names, sources and on/off
// state. The folders under load/<title id>/ are the truth: Reconcile drops records whose folder is
// gone and adopts folders it never saw, so mods copied by hand show up too.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eden_uwp/mods/mod_model.h"

namespace EdenXbox::Mods {

/// The records in `json`, in priority order; nullopt when it is not a manifest (corrupt or from a
/// newer version), so the caller rebuilds it from disk instead of trusting half of it.
std::optional<std::vector<ModRecord>> ParseManifest(std::string_view json);

std::string SerializeManifest(const std::vector<ModRecord>& mods);

/// Makes `mods` match `folders` (the mod folders on disk): records without a folder are dropped,
/// folders without a record are added at the end, on, as local mods. True when anything changed.
bool Reconcile(std::vector<ModRecord>& mods, const std::vector<std::string>& folders,
               std::int64_t now);

/// What the loader is told for one game (Settings::values.disabled_addons and mod_priority).
struct LoaderSettings {
    std::vector<std::string> disabled;
    std::vector<std::string> priority;
};

inline LoaderSettings ToLoaderSettings(const std::vector<ModRecord>& mods) {
    LoaderSettings settings;
    for (const auto& mod : mods) {
        settings.priority.push_back(mod.folder);
        if (!mod.enabled) settings.disabled.push_back(mod.folder);
    }
    return settings;
}

} // namespace EdenXbox::Mods
