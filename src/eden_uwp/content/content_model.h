// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace EdenXbox::Content {
enum class Kind { Update, Dlc };
enum class Error { None, Cancelled, Unavailable, Unsupported, Invalid, MissingKeys, Incomplete,
                   HashMismatch, WrongGame, Conflict, Space, Io, Registry, Requirements };
enum class ImportAction { Add, Duplicate, Conflict };
struct Revision {
    std::string id, source, name, display_version;
    std::uint64_t application{}, title{}, bytes{}, source_size{}, source_stamp{};
    std::uint32_t version{}, required_application{}, required_system{};
    Kind kind{};
    bool internal{}, verified{}, enabled{};
};
inline bool RequirementsSatisfied(const Revision& r, std::uint32_t application, std::uint32_t system) {
    return r.required_application <= application && r.required_system <= system;
}
inline bool ValidId(const std::string& id) {
    return id.size() == 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
inline ImportAction PlanImport(const std::vector<Revision>& catalog, const Revision& candidate) {
    for (const auto& item : catalog) {
        if (item.id == candidate.id) return ImportAction::Duplicate;
        if (item.title == candidate.title && item.version == candidate.version) return ImportAction::Conflict;
    }
    return ImportAction::Add;
}
// Selection is explicit. Adding an older revision never downgrades an active one.
inline void ActivateNew(std::vector<Revision>& catalog, Revision& added, bool have_base) {
    added.enabled = have_base && added.verified;
    for (auto& item : catalog) {
        const bool competes = added.kind == Kind::Update ?
            item.kind == Kind::Update && item.application == added.application : item.title == added.title;
        if (!competes || !item.enabled) continue;
        if (item.version >= added.version) added.enabled = false;
    }
    if (added.enabled) for (auto& item : catalog) {
        if ((added.kind == Kind::Update && item.kind == Kind::Update && item.application == added.application) ||
            (added.kind == Kind::Dlc && item.title == added.title)) item.enabled = false;
    }
}
inline Error Select(std::vector<Revision>& catalog, const std::string& id, bool enabled) {
    auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& r) { return r.id == id; });
    if (found == catalog.end()) return Error::Invalid;
    if (enabled && !found->verified) return Error::HashMismatch;
    if (enabled) for (auto& item : catalog) {
        if ((found->kind == Kind::Update && item.kind == Kind::Update && item.application == found->application) ||
            (found->kind == Kind::Dlc && item.title == found->title)) item.enabled = false;
    }
    found->enabled = enabled;
    return Error::None;
}
inline std::uint32_t EffectiveVersion(const std::vector<Revision>& catalog, std::uint64_t app,
                                      std::uint32_t base_version) {
    for (const auto& r : catalog)
        if (r.application == app && r.kind == Kind::Update && r.enabled) return r.version;
    return base_version;
}
} // namespace EdenXbox::Content
