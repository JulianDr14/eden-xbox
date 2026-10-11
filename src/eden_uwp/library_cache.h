// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Titles, developers and icons the library has already read, kept on disk so showing a page of
// covers is a few small file reads instead of opening every game through Eden's loaders.
//
// One file per game, named by a hash of its launch path. A record remembers the game file's
// size and write time when they are cheap to know (internal storage), so a replaced dump is read
// again, and the game language its title was read in, so changing it refreshes the titles.
// Writes go to a temporary file renamed over the old one: a crash never leaves half a record.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdenXbox {

struct LibraryCacheRecord {
    std::wstring name;
    std::wstring developer;
    std::vector<unsigned char> icon; ///< As the game ships it (JPEG), at most MaxIcon bytes.
    std::uint64_t program_id{};              ///< 0 when unknown (homebrew, unreadable game).
    std::array<std::uint8_t, 8> build_id{}; ///< The main module's, as cheat files are named.
    std::wstring display_version; ///< Effective NACP version, e.g. 2.0.0.
};

class LibraryCache {
public:
    static constexpr std::size_t MaxIcon = 1024 * 1024;

    explicit LibraryCache(std::filesystem::path directory);

    /// The record for `launch_path`, if its stamp and language match. Without `with_icon` only the
    /// texts are read (a header and three strings), for filling in names across the whole library.
    [[nodiscard]] std::optional<LibraryCacheRecord> Load(std::string_view launch_path,
                                                         std::uint64_t stamp, std::uint32_t language,
                                                         bool with_icon) const;
    bool Store(std::string_view launch_path, std::uint64_t stamp, std::uint32_t language,
               const LibraryCacheRecord& record) const;
    /// Deletes every record; returns false if some could not be removed.
    bool Clear() const;

    /// Identity of an internal game file (size and write time), 0 when unknown: external files
    /// are only reachable through brokered storage calls too slow to make per game per page.
    [[nodiscard]] static std::uint64_t StampFor(const std::filesystem::path& file);

    [[nodiscard]] const std::filesystem::path& Directory() const { return directory; }

private:
    [[nodiscard]] std::filesystem::path RecordPath(std::string_view launch_path) const;
    std::filesystem::path directory;
};

/// LocalCache\library in the app's data: kept across launches, not roamed or backed up.
std::filesystem::path DefaultLibraryCacheDirectory();

} // namespace EdenXbox
