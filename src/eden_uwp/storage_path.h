// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace EdenXbox {
inline constexpr std::string_view StoragePrefix = "eden-usb:/";
struct StoragePath {
    std::string token;
    std::string relative;
};
inline bool IsStoragePath(std::string_view path) {
    return path.starts_with(StoragePrefix);
}
// Paths are logical identities, never drive letters. Reject traversal/ADS and
// ambiguous separators before resolving anything through the authorized folder.
inline bool ValidStorageRelative(std::string_view relative) {
    if (relative.empty()) return true;
    if (relative.find_first_of("\\:\0", 0, 3) != std::string_view::npos) return false;
    while (!relative.empty()) {
        const auto slash = relative.find('/');
        const auto part = relative.substr(0, slash);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) return true;
        relative.remove_prefix(slash + 1);
        if (relative.empty()) return false;
    }
    return true;
}
inline std::optional<StoragePath> ParseStoragePath(std::string_view path) {
    if (!IsStoragePath(path)) return {};
    path.remove_prefix(StoragePrefix.size());
    const auto slash = path.find('/');
    const auto token = path.substr(0, slash);
    if (!token.starts_with("eden-games-") || token.size() > 100) return {};
    for (const unsigned char c : token)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '-') return {};
    const auto relative = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    if (!ValidStorageRelative(relative)) return {};
    return StoragePath{std::string{token}, std::string{relative}};
}
inline std::string MakeStoragePath(std::string_view token, std::string_view relative = {}) {
    return std::string{StoragePrefix} + std::string{token} +
           (relative.empty() ? "" : "/" + std::string{relative});
}
} // namespace EdenXbox
