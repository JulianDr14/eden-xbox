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

/// Makes upscaling Eden's scaling settings, which the D3D12 present reads every frame. On the GPU
/// thread while a game runs (GPU::RunOnGpuThread), the thread that reads them.
void ApplyUpscaling(Upscaling upscaling);

/// Renames the active profile to the saved user name (the default one when none is saved), before
/// a game starts. Returns false when the profile could not be written.
bool ApplyUserName(Service::Account::ProfileManager& profiles, std::wstring_view name);
} // namespace EdenXbox
