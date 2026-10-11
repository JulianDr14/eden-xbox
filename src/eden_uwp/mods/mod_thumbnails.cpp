// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_thumbnails.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "eden_uwp/atomic_file.h"
#include "eden_uwp/mods/mod_web.h"

namespace EdenXbox::Mods {
namespace {
namespace fs = std::filesystem;

/// A picture larger than this is not a thumbnail.
constexpr std::size_t MaxPicture = 4u << 20;

std::string Name(const fs::path& path) {
    const auto text = path.filename().u8string();
    return {text.begin(), text.end()};
}

} // namespace

ThumbnailCache::ThumbnailCache(fs::path folder_) : folder{std::move(folder_)} {
    std::error_code ec;
    fs::create_directories(folder, ec);
    for (fs::directory_iterator it{folder, ec}, end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) ready.insert(Name(it->path()));
}

fs::path ThumbnailCache::PathFor(const std::string& url) const {
    // FNV-1a: stable across runs, and collisions among a few thousand URLs are not a concern.
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : url) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    const auto dot = url.rfind('.');
    std::string extension = dot != std::string::npos && url.size() - dot <= 5 ? url.substr(dot) : ".img";
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
    return folder / (name + extension);
}

fs::path ThumbnailCache::Get(const std::string& url) const {
    if (url.empty() || !url.starts_with("https://")) return {};
    auto path = PathFor(url);
    const std::scoped_lock lock{mutex};
    if (ready.contains(Name(path))) return path;
    if (!requested.insert(url).second) return {};
    queue.Post([this, url, path = std::move(path)](std::stop_token stop) {
        auto body = Web::Get(url, stop, MaxPicture);
        if (!body || !WriteFileAtomically(path, *body)) return;
        const std::scoped_lock lock{mutex};
        ready.insert(Name(path));
        arrived = true;
    }, true);
    return {};
}

bool ThumbnailCache::TakeArrived() {
    return arrived.exchange(false);
}

void ThumbnailCache::Trim(const fs::path& folder, std::uint64_t budget) {
    struct File {
        fs::path path;
        fs::file_time_type time;
        std::uint64_t size;
    };
    std::vector<File> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it{folder, ec}, end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        files.push_back({it->path(), it->last_write_time(ec), it->file_size(ec)});
        total += files.back().size;
    }
    if (total <= budget) return;
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.time < b.time; });
    for (const auto& file : files) {
        if (total <= budget) break;
        if (fs::remove(file.path, ec)) total -= file.size;
    }
}

} // namespace EdenXbox::Mods
