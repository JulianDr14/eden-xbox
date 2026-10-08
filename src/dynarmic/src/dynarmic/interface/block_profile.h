// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: 0BSD

#pragma once

#include <cstdint>

namespace Dynarmic {

// Hash the exact instruction words consumed by Translate, in order. Addresses are
// excluded so profiles can follow the guest image's ASLR relocation.
// code_bytes is the span of guest code the block was translated from, counted
// from the descriptor's PC rounded down to a word (A32 Thumb may start mid-word).
struct BlockProfile {
    static constexpr std::uint32_t MaxCodeBytes = 64 * 1024;
    static constexpr std::uint64_t HashSeed = 14695981039346656037ULL;

    std::uint64_t descriptor{};
    std::uint64_t code_hash{HashSeed};
    std::uint32_t code_bytes{};

    /// Hashes one word without changing the span; Thumb reads a word once per halfword.
    void MixWord(std::uint32_t word) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            code_hash = (code_hash ^ ((word >> shift) & 0xff)) * 1099511628211ULL;
        }
    }

    /// One word of a stream that reads each word once, in order (A64, A32 ARM).
    void AddWord(std::uint32_t word) noexcept {
        MixWord(word);
        code_bytes += 4;
    }
};

} // namespace Dynarmic
