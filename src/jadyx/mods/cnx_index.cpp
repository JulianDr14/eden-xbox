// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/mods/cnx_index.h"

#include <algorithm>
#include <charconv>

#include <nlohmann/json.hpp>

namespace Jadyx::Mods::Cnx {
namespace {
using nlohmann::json;

bool SameHex(std::string_view a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

bool IsTitleId(std::string_view text) {
    return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

/// "https://.../0100000000ABC000.zip" -> "0100000000ABC000".
std::string_view LinkTitle(std::string_view url) {
    const auto slash = url.rfind('/');
    auto name = slash == std::string_view::npos ? url : url.substr(slash + 1);
    name = name.substr(0, name.find('.'));
    return IsTitleId(name) ? name : std::string_view{};
}

std::string_view EntryTitle(const json& entry) {
    const auto link = entry.find("link");
    if (link != entry.end() && link->is_string()) {
        if (const auto title = LinkTitle(link->get_ref<const std::string&>()); !title.empty()) return title;
    }
    const auto installed = entry.find("installed");
    if (installed == entry.end() || !installed->is_object()) return {};
    const auto file = installed->find("hash_file");
    if (file == installed->end() || !file->is_string()) return {};
    const std::string_view path = file->get_ref<const std::string&>();
    const auto first = path.substr(0, path.find('/'));
    return IsTitleId(first) ? first : std::string_view{};
}

} // namespace

std::vector<Entry> ParseIndex(std::string_view text, std::string_view title_id) {
    std::vector<Entry> entries;
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object()) return entries;
    const std::pair<const char*, Kind> sections[] = {{"translations", Kind::Translation},
                                                     {"modifications", Kind::Modification}};
    for (const auto& [section, kind] : sections) {
        const auto list = root.find(section);
        if (list == root.end() || !list->is_object()) continue;
        for (const auto& [name, entry] : list->items()) {
            if (!entry.is_object() || !entry.value("enabled", false)) continue;
            if (!SameHex(EntryTitle(entry), title_id)) continue;
            const auto link = entry.find("link");
            if (link == entry.end() || !link->is_string()) continue;
            const auto& url = link->get_ref<const std::string&>();
            if (!url.starts_with("https://")) continue;
            const auto size = entry.find("size");
            entries.push_back({name, kind, url, size != entry.end() && size->is_string() ? size->get<std::string>() : ""});
        }
    }
    return entries;
}

std::uint64_t ParseSize(std::string_view text) {
    double value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || value < 0) return 0;
    std::string_view unit{end, static_cast<std::size_t>(text.data() + text.size() - end)};
    while (!unit.empty() && unit.front() == ' ') unit.remove_prefix(1);
    const char first = unit.empty() ? 'B' : static_cast<char>(unit.front() & ~0x20);
    const double scale = first == 'K' ? 1024.0 : first == 'M' ? 1048576.0 : first == 'G' ? 1073741824.0 : 1.0;
    return static_cast<std::uint64_t>(value * scale);
}

} // namespace Jadyx::Mods::Cnx
