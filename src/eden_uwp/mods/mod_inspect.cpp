// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_inspect.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace EdenXbox::Mods {
namespace {
namespace fs = std::filesystem;

std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string Lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}

/// Every regular file under `root`, with its path relative to it ('/'-separated, lower case).
template <typename Visit>
void ForEachFile(const fs::path& root, Visit visit) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it{root, ec}, end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        visit(it->path(), Lowered(Utf8(it->path().lexically_relative(root))));
    }
}

/// The loader's folders are matched without case (FindSubdirectoryCaseless).
fs::path Child(const fs::path& folder, std::string_view name) {
    std::error_code ec;
    for (fs::directory_iterator it{folder, ec}, end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && Lowered(Utf8(it->path().filename())) == name) return it->path();
    return {};
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

void AddBuildId(ModContents& contents, std::string id) {
    if (!id.empty() && std::find(contents.build_ids.begin(), contents.build_ids.end(), id) ==
                           contents.build_ids.end())
        contents.build_ids.push_back(std::move(id));
}

} // namespace

std::string ShortBuildId(std::string_view text) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }))
        return {};
    std::string id{text.substr(0, 16)};
    id.resize(16, '0');
    std::transform(id.begin(), id.end(), id.begin(),
                   [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    return id;
}

std::string PchtxtBuildId(std::string_view text) {
    constexpr std::string_view tag = "@nsobid-";
    const auto at = text.find(tag);
    if (at == std::string_view::npos) return {};
    auto end = at + tag.size();
    while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end]))) ++end;
    return ShortBuildId(text.substr(at + tag.size(), end - at - tag.size()));
}

ModContents InspectFolder(const fs::path& folder) {
    ModContents contents;
    std::error_code ec;
    ForEachFile(folder, [&](const fs::path& path, const std::string&) {
        const auto bytes = fs::file_size(path, ec);
        contents.bytes += ec ? 0 : bytes;
        contents.listing.push_back({Utf8(path.lexically_relative(folder)), ec ? 0 : bytes});
    });
    std::sort(contents.listing.begin(), contents.listing.end(),
              [](const ModFileEntry& a, const ModFileEntry& b) { return a.path < b.path; });
    for (const auto name : {"romfs", "romfslite"}) {
        const auto romfs = Child(folder, name);
        if (romfs.empty()) continue;
        ForEachFile(romfs, [&](const fs::path&, const std::string& relative) {
            contents.parts |= RomFs;
            contents.files.push_back("romfs/" + relative);
        });
    }
    // romfs_ext: <file>.stub deletes a game file and <file>.ips patches one; both claim it.
    if (const auto ext = Child(folder, "romfs_ext"); !ext.empty()) {
        ForEachFile(ext, [&](const fs::path&, const std::string& relative) {
            contents.parts |= RomFsExt;
            if (EndsWith(relative, ".stub") || EndsWith(relative, ".ips"))
                contents.files.push_back("romfs/" + relative.substr(0, relative.rfind('.')));
        });
    }
    if (const auto exefs = Child(folder, "exefs"); !exefs.empty()) {
        ForEachFile(exefs, [&](const fs::path& path, const std::string& relative) {
            if (EndsWith(relative, ".ips")) {
                contents.parts |= Patch;
                AddBuildId(contents, ShortBuildId(relative.substr(0, relative.find('.'))));
            } else if (EndsWith(relative, ".pchtxt")) {
                contents.parts |= Patch;
                std::ifstream in{path, std::ios::binary};
                std::string head(4096, '\0');
                in.read(head.data(), static_cast<std::streamsize>(head.size()));
                head.resize(static_cast<std::size_t>(in.gcount()));
                AddBuildId(contents, PchtxtBuildId(head));
            } else {
                contents.parts |= ExeFs;
                contents.files.push_back("exefs/" + relative);
            }
        });
    }
    if (const auto cheats = Child(folder, "cheats"); !cheats.empty()) {
        ForEachFile(cheats, [&](const fs::path&, const std::string& relative) {
            if (!EndsWith(relative, ".txt") || relative == "toggles.txt") return;
            contents.parts |= Cheats;
            AddBuildId(contents, ShortBuildId(relative.substr(0, relative.size() - 4)));
        });
    }
    std::sort(contents.files.begin(), contents.files.end());
    contents.files.erase(std::unique(contents.files.begin(), contents.files.end()), contents.files.end());
    return contents;
}

} // namespace EdenXbox::Mods
