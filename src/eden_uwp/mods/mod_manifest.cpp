// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_manifest.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace EdenXbox::Mods {
namespace {
using nlohmann::json;

constexpr int SchemaVersion = 1;

constexpr std::string_view SourceNames[] = {"local", "archive", "gamebanana", "cnx"};

std::string_view SourceName(SourceKind kind) {
    return SourceNames[static_cast<std::size_t>(kind)];
}

SourceKind SourceFromName(std::string_view name) {
    for (std::size_t i = 0; i < std::size(SourceNames); ++i)
        if (SourceNames[i] == name) return static_cast<SourceKind>(i);
    return SourceKind::Local;
}

/// Folder names are compared like Windows compares them: without case.
bool SameFolder(std::string_view a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

} // namespace

std::optional<std::vector<ModRecord>> ParseManifest(std::string_view text) {
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("version", 0) != SchemaVersion) return std::nullopt;
    const auto list = root.find("mods");
    if (list == root.end() || !list->is_array()) return std::nullopt;
    std::vector<ModRecord> mods;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        ModRecord mod;
        try {
            mod.folder = item.value("folder", std::string{});
            mod.name = item.value("name", mod.folder);
            mod.enabled = item.value("enabled", true);
            mod.installed = item.value("installed", std::int64_t{});
            if (const auto source = item.find("source"); source != item.end() && source->is_object()) {
                mod.source.kind = SourceFromName(source->value("kind", std::string{}));
                mod.source.archive = source->value("archive", std::string{});
                mod.source.mod_id = source->value("mod", std::int64_t{});
                mod.source.file_id = source->value("file", std::int64_t{});
                mod.source.version = source->value("version", std::string{});
                mod.source.updated = source->value("updated", std::int64_t{});
                mod.source.url = source->value("url", std::string{});
            }
        } catch (const json::exception&) {
            continue; // A field of the wrong type: skip the record, keep the rest.
        }
        if (mod.folder.empty()) continue;
        const bool duplicate = std::any_of(mods.begin(), mods.end(),
                                           [&](const ModRecord& m) { return SameFolder(m.folder, mod.folder); });
        if (!duplicate) mods.push_back(std::move(mod));
    }
    return mods;
}

std::string SerializeManifest(const std::vector<ModRecord>& mods) {
    json list = json::array();
    for (const auto& mod : mods) {
        list.push_back({
            {"folder", mod.folder},
            {"name", mod.name},
            {"enabled", mod.enabled},
            {"installed", mod.installed},
            {"source",
             {{"kind", SourceName(mod.source.kind)},
              {"archive", mod.source.archive},
              {"mod", mod.source.mod_id},
              {"file", mod.source.file_id},
              {"version", mod.source.version},
              {"updated", mod.source.updated},
              {"url", mod.source.url}}},
        });
    }
    // Names come from archives and the web: never let one invalid byte lose the whole file.
    return json{{"version", SchemaVersion}, {"mods", std::move(list)}}
        .dump(2, ' ', false, json::error_handler_t::replace);
}

bool Reconcile(std::vector<ModRecord>& mods, const std::vector<std::string>& folders,
               std::int64_t now) {
    const auto before = mods.size();
    std::erase_if(mods, [&](const ModRecord& mod) {
        return std::none_of(folders.begin(), folders.end(),
                            [&](const std::string& folder) { return SameFolder(folder, mod.folder); });
    });
    bool changed = mods.size() != before;
    for (const auto& folder : folders) {
        const bool known = std::any_of(mods.begin(), mods.end(),
                                       [&](const ModRecord& mod) { return SameFolder(mod.folder, folder); });
        if (known) continue;
        ModRecord adopted;
        adopted.folder = folder;
        adopted.name = folder;
        adopted.installed = now;
        mods.push_back(std::move(adopted));
        changed = true;
    }
    return changed;
}

} // namespace EdenXbox::Mods
