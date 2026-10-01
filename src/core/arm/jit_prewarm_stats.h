// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Core::JitPrewarm {

enum class Miss : std::size_t {
    Unlearned, Budget, OtherCore, FpcrVariant, CodeChanged, Rejected,
    Recompiled, RecordOnly, OutsideExecutable, Count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(Miss::Count)> MissNames{
    "unlearned", "budget", "other-core", "fpcr-variant", "code-changed",
    "prewarm-rejected", "warmed-recompiled", "record-only", "outside-static-rx"
};
using MissSnapshot = std::array<std::array<std::uint64_t, MissNames.size()>, 4>;
inline std::array<std::array<std::atomic<std::uint64_t>, MissNames.size()>, 4> miss_counters{};
inline std::atomic_bool capture_active{};

// Only compilation callbacks update these counters; no work on cached block execution.
inline MissSnapshot ReadMisses() {
    MissSnapshot result{};
    for (std::size_t core = 0; core < result.size(); ++core) {
        for (std::size_t reason = 0; reason < MissNames.size(); ++reason) {
            result[core][reason] = miss_counters[core][reason].load(std::memory_order_relaxed);
        }
    }
    return result;
}

} // namespace Core::JitPrewarm
