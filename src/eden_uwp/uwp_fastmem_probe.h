// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>

namespace EdenXbox {

using ProbeLog = std::function<void(const std::string&)>;
using ProbeMemory = std::function<std::string()>;

// Checks, before the emulator starts, whether the console allows the memory layout fastmem needs
// (see uwp_fastmem_probe.cpp). Logs one line per step and frees everything it reserved.
void ProbeFastmem(const ProbeLog& log, const ProbeMemory& memory);

} // namespace EdenXbox
