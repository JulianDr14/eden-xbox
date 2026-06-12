// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string_view>

namespace Kernel::Svc {

/// Observer invoked with each guest `svcOutputDebugString` chunk as it is received.
using DebugStringObserver = std::function<void(std::string_view)>;

/// Install (or clear, with a default-constructed observer) a hook that is called with every guest
/// debug-string write. Used by the headless Xbox/UWP boot frontend to detect the JIT-liveness
/// sentinel and terminate deterministically once the guest has demonstrably executed code through
/// the JIT. Unset (the default) is a no-op with no per-call cost beyond a flag check. Thread-safe.
void SetDebugStringObserver(DebugStringObserver observer);

} // namespace Kernel::Svc
