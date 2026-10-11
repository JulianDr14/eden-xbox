// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Small files the frontend keeps (cache records, cheat toggles): read whole, and replaced through
// a temporary file so a crash or a full disk never leaves half a file behind.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace EdenXbox {

inline std::optional<std::string> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// Replaces `path` with `bytes`, creating its folder. False when it could not be written.
inline bool WriteFileAtomically(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())).flush()) {
            out.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) std::filesystem::remove(temporary, ec);
    return !ec;
}

} // namespace EdenXbox
