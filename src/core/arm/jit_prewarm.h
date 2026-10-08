// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>

namespace Core {
class System;

/// Code each core may warm. Each MiB of warmed code costs about three of commit (dynarmic's
/// block tables), so a frontend with a tight memory limit passes less (see EdenXbox's
/// PrewarmBudget); the plan warms gameplay and shared blocks first.
inline constexpr std::size_t DefaultPrewarmCodeBudget = 64 * 1024 * 1024;

// Frontend entry point. Must run after Load and before System::Run. Covers A64 and A32
// applications alike (one profile per core and ISA, core/arm/dynarmic/jit_prewarm_owner.h).
void ConfigureApplicationPrewarm(System& system, bool warm,
                                const std::function<void(std::size_t, std::size_t)>& progress,
                                std::size_t code_budget = DefaultPrewarmCodeBudget);
}
