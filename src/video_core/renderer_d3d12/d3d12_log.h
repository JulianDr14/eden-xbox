// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <utility>

#include <fmt/format.h>

#include "common/bug_tracker.h"
#include "common/logging.h"

namespace D3D12 {

/// Logs a "D3D12: ..." warning the first time only. The caller's BUG_TRACK counts every occurrence,
/// so the bug tracker does not count this log line again. The message is formatted only once.
template <typename... Args>
void WarnOnceLog(bool& logged, fmt::format_string<Args...> format, Args&&... args) {
    if (!logged) {
        logged = true;
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_WARNING(Render, "D3D12: {}", fmt::format(format, std::forward<Args>(args)...));
    }
}

/// WarnOnceLog for flags several threads share. The load first keeps repeated calls (some are per
/// draw) from issuing a locked exchange every time.
template <typename... Args>
void WarnOnceLog(std::atomic_bool& logged, fmt::format_string<Args...> format, Args&&... args) {
    if (!logged.load(std::memory_order_relaxed) &&
        !logged.exchange(true, std::memory_order_relaxed)) {
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_WARNING(Render, "D3D12: {}", fmt::format(format, std::forward<Args>(args)...));
    }
}

} // namespace D3D12
