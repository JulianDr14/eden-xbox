// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace EdenXbox {
// Runs on the CoreWindow thread, before constructing the guest renderer. Returns
// a UTF-8 path relative to LocalState/games, or nullopt when the user exits.
// The seeding callback and directory scan run off the UI thread.
std::optional<std::string> ShowGameLibrary(void* core_window, unsigned width, unsigned height,
                                         const std::filesystem::path& root,
                                         const std::function<void()>& seed);
} // namespace EdenXbox
