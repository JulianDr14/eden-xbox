// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reading .zip, .7z and .rar (RAR4 and RAR5) archives through libarchive's readers. The bytes come
// from ArchiveSource, so an archive is read where it is (internal storage or a USB drive through the
// VFS) without copying it first; a split archive (name.part1.rar..., name.7z.001...) is its volumes
// in order. Extraction writes only the files a layout picked (mod_layout.h), each through a
// temporary name, and stops at the size limit it is given, so a hostile archive can neither write
// outside its folder nor fill the disk.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "eden_uwp/mods/mod_layout.h"

namespace EdenXbox::Mods {

/// Random-access bytes an archive volume is read from.
class ArchiveSource {
public:
    virtual ~ArchiveSource() = default;
    [[nodiscard]] virtual std::uint64_t Size() const = 0;
    /// Reads up to `size` bytes at `offset`; returns how many (0 at the end or on failure).
    virtual std::size_t Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) = 0;
};

/// A file on a path std::filesystem can open; null when it cannot be opened.
std::unique_ptr<ArchiveSource> OpenFileSource(const std::filesystem::path& path);

enum class ArchiveError : std::uint8_t {
    None,
    Unreadable, ///< Not an archive, damaged, or a volume is missing.
    Encrypted,  ///< Protected with a password.
    TooLarge,   ///< More than the limit given (or a hostile size).
    Write,      ///< The destination could not be written (space, permissions).
    Cancelled,
};

struct ArchiveListing {
    ArchiveError error{};
    std::vector<ArchiveItem> items; ///< Regular files only, with their declared sizes.
    std::string format;             ///< libarchive's name for it ("7-Zip", "RAR5"...).
    std::string detail;             ///< libarchive's message when it failed, for the log.
};

using ArchiveProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

/// Lists the files of the archive made of `volumes`.
ArchiveListing ListArchive(std::vector<std::unique_ptr<ArchiveSource>>& volumes,
                           std::stop_token stop = {});

/// Writes `files` (from ListArchive's paths to paths under `destination`) and nothing else.
/// Stops with TooLarge once more than `max_bytes` would be written in total.
ArchiveError ExtractArchive(std::vector<std::unique_ptr<ArchiveSource>>& volumes,
                            const std::vector<LayoutFile>& files,
                            const std::filesystem::path& destination, std::uint64_t max_bytes,
                            std::stop_token stop = {}, const ArchiveProgress& progress = {},
                            std::string* detail = nullptr);

} // namespace EdenXbox::Mods
