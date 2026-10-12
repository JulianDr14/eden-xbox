// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace EdenXbox {
struct LibraryEntry {
    std::filesystem::path relative_path;
    std::wstring name;
    std::wstring developer;
    std::vector<unsigned char> icon;
    bool metadata_loaded{}; ///< Title and icon requested for the visible window.
    bool title_loaded{};    ///< Title and developer known (from the game or the cache).
    std::string launch_path; // Absolute internal path or token-scoped external identity.
    std::wstring source_name;
    std::uint64_t program_id{};              ///< Known with the title; 0 for homebrew.
    std::array<std::uint8_t, 8> build_id{}; ///< The main module's, as cheat files are named.
    std::wstring display_version; ///< Effective NACP version, e.g. 2.0.0.
    std::wstring pulse_saved; ///< When the kept Pulse session was saved; empty without one.
    bool pulse_current{};     ///< That session was made by this build, so it can resume.
};
struct LibraryScan {
    std::vector<LibraryEntry> entries;
    std::string error;
    bool limited{};
    bool missing_root{}; ///< The internal games folder does not exist.
};

// Only enumerate names: never open multi-gigabyte containers during a scan.
// Skip links, cap traversal, and tolerate inaccessible entries in the sandbox.
inline LibraryScan ScanGameLibrary(const std::filesystem::path& root,
                                   std::stop_token stop = {}) {
    LibraryScan result;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        // Not an error by itself: games can all live in external folders.
        result.missing_root = true;
        if (ec) result.error = ec.message();
        return result;
    }
    auto it = std::filesystem::recursive_directory_iterator{
        root, std::filesystem::directory_options::skip_permission_denied, ec};
    const auto end = std::filesystem::recursive_directory_iterator{};
    unsigned visited = 0;
    for (; !ec && it != end && !stop.stop_requested(); it.increment(ec)) {
        if (++visited > 10000) {
            result.limited = true;
            break;
        }
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (std::filesystem::is_symlink(status)) {
            it.disable_recursion_pending();
            continue;
        }
        if (it.depth() >= 4) it.disable_recursion_pending();
        if (!std::filesystem::is_regular_file(status)) continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".nsp" && ext != ".xci" && ext != ".nro") continue;
        result.entries.push_back({it->path().lexically_relative(root),
                                  it->path().stem().wstring()});
    }
    if (ec) result.error = ec.message();
    std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
        return a.relative_path < b.relative_path;
    });
    return result;
}
} // namespace EdenXbox
