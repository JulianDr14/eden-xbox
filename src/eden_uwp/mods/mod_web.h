// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mod store's HTTP: small GETs for the API and pictures, an ETag-cached GET for the CNX index,
// and file downloads that resume. A download writes to a .part file and asks the server for the
// rest of it (Range) when it starts again, after a dropped connection or a closed page; the MD5 the
// site publishes is checked over the whole file before anything installs it.
//
// Every call blocks: run them on a WorkQueue (multithreaded apartment). The stop token ends them
// within a fraction of a second.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace EdenXbox::Mods::Web {

/// LocalCache\mods: what the store downloads (pictures, archives, the CNX index). Clearing it
/// loses nothing a page cannot download again.
std::filesystem::path CacheFolder();

/// The body of a 200 answer, or nullopt (no connection, another status, more than `max_bytes`).
std::optional<std::string> Get(const std::string& url, std::stop_token stop,
                               std::size_t max_bytes = 8u << 20);

/// Like Get, keeping the body and its ETag in `file`: within `recheck_minutes` of the last check
/// the copy is used as it is, then the server is asked with If-None-Match. Without a connection
/// the copy is used however old it is.
std::optional<std::string> GetCached(const std::string& url, const std::filesystem::path& file,
                                     int recheck_minutes, std::stop_token stop);

enum class DownloadError : std::uint8_t {
    None,
    Network,   ///< No connection, a timeout or a failed answer: worth trying again.
    Checksum,  ///< The file is not the one the site describes.
    TooLarge,  ///< Larger than the space allowed.
    Write,
    Cancelled,
};

using DownloadProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

/// Downloads `url` into `part`, resuming what it already holds. `md5` (32 lower-case hex digits,
/// or empty) is checked when it ends; a mismatch deletes the file. `max_bytes` caps the size.
DownloadError Download(const std::string& url, const std::filesystem::path& part, const std::string& md5,
                       std::uint64_t max_bytes, std::stop_token stop, const DownloadProgress& progress,
                       std::string& detail);

} // namespace EdenXbox::Mods::Web
