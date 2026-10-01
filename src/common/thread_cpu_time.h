// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include "common/common_types.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {
/// OS-accounted user+kernel CPU time of this host thread. Not wall time or guest-only time.
/// FILETIME's 100 ns units do not imply 100 ns measurement resolution. Unsupported/failure = null.
inline std::optional<u64> CurrentThreadCpuTimeNs() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return std::nullopt;
    const auto value = [](FILETIME t) {
        return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return (value(kernel) + value(user)) * 100;
#else
    return std::nullopt;
#endif
}
} // namespace Common
