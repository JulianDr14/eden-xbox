// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: 0BSD

#pragma once

#include <cstdint>

namespace Dynarmic {

// Hash the exact instruction words consumed by Translate, in order. Addresses are
// excluded so profiles can follow the guest image's ASLR relocation.
struct BlockProfile {
    static constexpr std::uint32_t MaxCodeBytes = 64 * 1024;
    static constexpr std::uint64_t HashSeed = 14695981039346656037ULL;

    std::uint64_t descriptor{};
    std::uint64_t code_hash{HashSeed};
    std::uint32_t code_bytes{};

    void AddWord(std::uint32_t word) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            code_hash = (code_hash ^ ((word >> shift) & 0xff)) * 1099511628211ULL;
        }
        code_bytes += 4;
    }
};

} // namespace Dynarmic
