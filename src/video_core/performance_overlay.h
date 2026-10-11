// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>

namespace VideoCore {
// Frontend preference shared with the render thread; disabled until settings are loaded.
inline std::atomic<bool> performance_overlay_enabled{false};
} // namespace VideoCore
