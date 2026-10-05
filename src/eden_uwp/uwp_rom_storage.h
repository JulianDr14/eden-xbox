// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>
#include <winrt/Windows.Storage.h>
#include "core/file_sys/vfs/vfs_types.h"
#include "eden_uwp/game_library.h"

namespace EdenXbox {
// FutureAccessList is the persisted source catalog. Unavailable tokens remain
// there so reconnecting a USB does not silently forget the user's library.
std::string RememberGameFolder(const winrt::Windows::Storage::StorageFolder& folder);
LibraryScan ScanExternalGameFolders(std::stop_token stop);
FileSys::VirtualFilesystem MakeUwpFilesystem();
// Bounded preflight, run on an MTA worker before starting the guest. Reports
// access/removal errors in the library instead of closing the application.
std::string CheckExternalGame(std::string_view path);
// Opt-in AppContainer gate against a sparse fixture prepared by rom-storage-gate.ps1.
bool RunRomStorageGate(bool restore, const std::function<void(std::string)>& diagnostic);
} // namespace EdenXbox
