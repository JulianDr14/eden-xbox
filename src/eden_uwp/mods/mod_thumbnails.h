// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The store's pictures on disk (LocalCache\mods\pictures), named after a hash of their URL. A page
// asks for the pictures it shows as it draws; a missing one is downloaded in the background, the
// latest asked first, so scrolling fetches what is on screen before what was. The folder is kept
// under a size budget, oldest pictures out first, when a store opens.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

#include "eden_uwp/mods/work_queue.h"

namespace EdenXbox::Mods {

class ThumbnailCache {
public:
    explicit ThumbnailCache(std::filesystem::path folder);

    /// The picture's file once it is on disk; empty while it downloads (or when it cannot).
    /// Const for the views that draw: asking for a picture is not a change to the page.
    std::filesystem::path Get(const std::string& url) const;

    /// True once since the last call when a picture arrived: the page is worth drawing again.
    bool TakeArrived();

    /// Deletes the oldest pictures until the folder holds at most `budget` bytes.
    static void Trim(const std::filesystem::path& folder, std::uint64_t budget);

private:
    std::filesystem::path PathFor(const std::string& url) const;

    std::filesystem::path folder;
    mutable std::mutex mutex;
    mutable std::unordered_set<std::string> ready;     ///< File names on disk.
    mutable std::unordered_set<std::string> requested; ///< Asked for or failed this session.
    mutable std::atomic<bool> arrived{};
    mutable WorkQueue queue; ///< Last: its thread stops before the rest goes.
};

} // namespace EdenXbox::Mods
