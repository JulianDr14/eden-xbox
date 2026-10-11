// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The store's downloads: a queue that fetches one archive at a time into LocalCache\mods\downloads
// while the player keeps browsing. A file is named after its source and id, so a download cut short
// (a lost connection, a closed page) resumes where it stopped the next time it is asked for. A lost
// connection is retried a few times, waiting longer each time; a finished file waits for the page
// to install it.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "eden_uwp/mods/mod_installer.h"
#include "eden_uwp/mods/work_queue.h"

namespace EdenXbox::Mods {

struct DownloadRequest {
    std::string key;   ///< Unique per file ("gb-815939"): names the file on disk.
    std::string title; ///< What the player sees ("Mod name · file.zip").
    std::string name;  ///< The mod's name, for the mods it installs.
    std::string url;
    std::string md5;   ///< Empty when the source gives none.
    std::uint64_t size{}; ///< Expected, for the space check; 0 when unknown.
    ModSource source;
    std::string replace; ///< The installed mod's folder this updates; empty for a new mod.
};

struct DownloadStatus {
    std::string key;
    std::string title;
    bool running{};
    std::uint64_t done{};
    std::uint64_t total{};
    std::uint64_t speed{}; ///< Bytes per second, over the last seconds.
};

struct FinishedDownload {
    DownloadRequest request;
    std::filesystem::path file; ///< The archive, when it downloaded.
    ModError error{};
};

class ModDownloads {
public:
    explicit ModDownloads(std::filesystem::path folder);
    ~ModDownloads();

    /// Queues a download; one already queued or running for the same file is left as it is.
    void Enqueue(DownloadRequest request);
    /// Stops the running download (its part stays for later) and drops the queue.
    void CancelAll();

    /// Queued and running downloads, the running one first.
    [[nodiscard]] std::vector<DownloadStatus> Status() const;
    [[nodiscard]] bool Active() const;
    std::optional<FinishedDownload> TakeFinished();

    /// Deletes downloads nobody resumed for a week, and finished files left by a crash.
    static void Trim(const std::filesystem::path& folder);

private:
    void Run(DownloadRequest request, std::stop_token stop);

    std::filesystem::path folder;
    mutable std::mutex mutex;
    std::vector<DownloadStatus> status;
    std::deque<FinishedDownload> finished;
    std::stop_source cancel;
    WorkQueue queue; ///< Last: its thread stops before the rest goes.
};

} // namespace EdenXbox::Mods
