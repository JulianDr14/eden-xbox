// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>

namespace Core {
class System;

// Frontend entry point. Must run after Load and before System::Run.
void ConfigureApplicationPrewarm(System& system, bool warm,
                                const std::function<void(std::size_t, std::size_t)>& progress);
}
