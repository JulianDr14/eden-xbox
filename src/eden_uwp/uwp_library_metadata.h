// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "eden_uwp/game_library.h"
namespace EdenXbox {
// Reuse the same loaders as qt_common/game_list/worker.cpp. Only requested
// visible entries are inspected; there is no guest Initialize/Load/Run.
void ReadLibraryMetadata(const std::filesystem::path& root,
                         std::vector<LibraryEntry>& entries, std::stop_token stop);
}
