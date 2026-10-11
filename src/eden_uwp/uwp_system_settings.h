// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "eden_uwp/system_settings.h"

namespace Service::Account {
class ProfileManager;
}

namespace EdenXbox {
/// Saved in the app's LocalSettings, like the controller options.
SystemSettings LoadSystemSettings();
bool SaveSystemSettings(const SystemSettings& settings);

/// Windows' preferred languages and home region (the console's, on Xbox). Never throws.
SystemLocale DetectSystemLocale();

/// Resolves the saved settings against the system and makes the result current: the interface
/// language for the frontend, and Eden's system language and region for the next game.
ResolvedSystem ApplySystemSettings();

/// Renames the active profile to the saved user name, before a game starts. Returns false when
/// nothing was saved or the profile could not be written.
bool ApplyUserName(Service::Account::ProfileManager& profiles, std::wstring_view name);
} // namespace EdenXbox
