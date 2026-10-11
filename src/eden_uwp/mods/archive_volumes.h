// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The archives in a folder, with split ones gathered: name.part1.rar, name.part2.rar...; the older
// name.rar, name.r00, name.r01...; and name.7z.001, name.zip.001... Each archive is one row, its
// volumes in the order they are read. Anything else is not an archive the manager opens.

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdenXbox::Mods {

/// A file of a folder: its name (UTF-8) and how to open it.
struct FolderFile {
    std::string name;
    std::string path;
};

/// One archive: its name as shown and its volumes in order.
struct ArchiveGroup {
    std::string name;
    std::vector<std::string> volumes;
};

namespace Detail {

inline std::string Lower(std::string_view text) {
    std::string out{text};
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

/// A volume number: a few digits (never enough to overflow).
inline bool Digits(std::string_view text) {
    return !text.empty() && text.size() <= 6 &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// Which archive a file name belongs to, and its place in it.
struct Volume {
    std::string key;  ///< The archive's name, lower case.
    std::string name; ///< The archive's name as shown.
    long order{};
};

inline std::optional<Volume> ParseVolume(std::string_view name) {
    const std::string lower = Lower(name);
    const auto ends = [&](std::string_view suffix) { return lower.ends_with(suffix); };
    // name.partN.rar
    if (ends(".rar")) {
        const auto stem = lower.substr(0, lower.size() - 4);
        const auto part = stem.rfind(".part");
        if (part != std::string::npos && Digits(std::string_view{stem}.substr(part + 5))) {
            const std::string shown = std::string{name.substr(0, part)} + ".rar";
            return Volume{Lower(shown), shown, std::stol(stem.substr(part + 5))};
        }
        return Volume{lower, std::string{name}, -1}; // name.rar before its .r00, .r01...
    }
    const auto dot = lower.rfind('.');
    if (dot == std::string::npos) return std::nullopt;
    const std::string_view extension = std::string_view{lower}.substr(dot + 1);
    // name.r00, name.r01... follow name.rar.
    if (extension.size() == 3 && extension[0] == 'r' && Digits(extension.substr(1))) {
        const std::string shown = std::string{name.substr(0, dot)} + ".rar";
        return Volume{Lower(shown), shown, std::stol(std::string{extension.substr(1)})};
    }
    // name.7z.001, name.zip.001...
    if (Digits(extension)) {
        const auto inner = lower.substr(0, dot);
        if (inner.ends_with(".7z") || inner.ends_with(".zip") || inner.ends_with(".rar")) {
            const std::string shown{name.substr(0, dot)};
            return Volume{Lower(shown), shown, std::stol(std::string{extension})};
        }
        return std::nullopt;
    }
    if (extension == "zip" || extension == "7z") return Volume{lower, std::string{name}, 0};
    return std::nullopt;
}

} // namespace Detail

/// The archives among `files`, sorted by name.
inline std::vector<ArchiveGroup> GroupArchives(const std::vector<FolderFile>& files) {
    std::map<std::string, std::pair<std::string, std::vector<std::pair<long, std::string>>>> groups;
    for (const auto& file : files) {
        const auto volume = Detail::ParseVolume(file.name);
        if (!volume) continue;
        auto& group = groups[volume->key];
        if (group.first.empty() || volume->order < 0) group.first = volume->name;
        group.second.emplace_back(volume->order, file.path);
    }
    std::vector<ArchiveGroup> out;
    for (auto& [key, group] : groups) {
        auto& volumes = group.second;
        std::sort(volumes.begin(), volumes.end());
        ArchiveGroup archive{group.first, {}};
        for (auto& volume : volumes) archive.volumes.push_back(std::move(volume.second));
        out.push_back(std::move(archive));
    }
    return out;
}

} // namespace EdenXbox::Mods
