// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <windows.h>
#include <psapi.h>
#include <fmt/format.h>

#include "common/common_types.h"

namespace D3D12 {

// Resident private + shared pages, not private commit or reserved address space.
// K32GetProcessMemoryInfo is available to desktop and UWP apps; no extra library.
inline std::optional<u64> QueryOverlayResidentMemory(HANDLE process = GetCurrentProcess()) {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
        return std::nullopt;
    }
    return static_cast<u64>(counters.WorkingSetSize);
}

inline std::string FormatOverlayResidentMemory(std::optional<u64> bytes) {
    if (!bytes) {
        return "RAM WS --";
    }
    constexpr u64 MiB = 1024ULL * 1024;
    constexpr u64 GiB = 1024 * MiB;
    return *bytes < GiB ? fmt::format("RAM WS {:.0f} MIB", static_cast<double>(*bytes) / MiB)
                        : fmt::format("RAM WS {:.2f} GIB", static_cast<double>(*bytes) / GiB);
}

} // namespace D3D12
