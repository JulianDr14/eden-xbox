// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pulse sessions (docs/xbox/xbox_pulse.md, phase 4): "Back to home" captures the running game and
// picking it again in the library resumes it. One session per game file and user, kept in
// LocalState/eden/pulse/sessions under the Store's quota; a session is consumed by the restore
// that uses it and is never deleted without the player asking. Nothing runs while playing: the
// capture happens only when the game is left.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "common/pulse_archive.h"
#include "core/hle/kernel/pulse_process_layout.h"

namespace Core {
class System;
}

namespace EdenXbox::PulseSessions {

/// Saved with the system settings ("Pulse sessions", on by default).
bool Enabled();

/// The identity of `launch_path` as booted: the slot (the file), the profile user, what the file
/// and its installed content are, and this build. Any difference makes a session incompatible.
Common::Pulse::Identity IdentityFor(std::string_view launch_path, std::span<const std::uint8_t, 16> user,
                                    std::uint64_t file_size, std::uint64_t application,
                                    std::uint32_t version, std::span<const std::uint8_t, 8> build_id);

/// The session this exact identity can resume, if one is available.
bool CanResume(const Common::Pulse::Identity& identity);

/// Resumes `identity`'s session into a fresh, stopped `system`. `load(pin)` boots the game with
/// the layout pin set, without running it. Throws on any failure: the System is then discarded
/// (when `load` ran) and the session stays where it was.
void Resume(Core::System& system, const Common::Pulse::Identity& identity,
            const std::function<void(const Kernel::ProcessLayoutPin&)>& load);

/// Captures the running (or paused) game and publishes it. Empty on success, else why not. The
/// system is left paused either way, ready to shut down.
std::string Save(Core::System& system, const Common::Pulse::Identity& identity);

/// What the library shows for a game file: the newest session kept for it.
struct Kept {
    std::filesystem::file_time_type saved{};
    bool current{}; ///< Made by this build; otherwise it cannot be resumed.
};
std::optional<Kept> Find(std::string_view launch_path);
/// The player discards the game's sessions. Normal saves are never touched.
bool Discard(std::string_view launch_path);
/// "2026-10-12 18:30", local time.
std::wstring FormatTime(std::filesystem::file_time_type time);

/// A message for the library when it opens next (session saved, or why not).
void SetNotice(std::string text);
std::string TakeNotice();

} // namespace EdenXbox::PulseSessions
