// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
namespace Common {
// Regular SSE2 stores, never read destination and never use non-temporal instructions.
// Benchmark candidate only: guest memory translation/read/invalidation is not bypassed.
inline void CopyToWriteCombined(void* destination, const void* source, size_t size) {
#if defined(_M_X64) || defined(__x86_64__)
    if (size <= 512 && (reinterpret_cast<uintptr_t>(destination) & 15) == 0) {
        auto* dst = static_cast<unsigned char*>(destination);
        auto* src = static_cast<const unsigned char*>(source);
        while (size >= 64) {
            const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 16));
            const auto c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 32));
            const auto d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 48));
            _mm_store_si128(reinterpret_cast<__m128i*>(dst), a);
            _mm_store_si128(reinterpret_cast<__m128i*>(dst + 16), b);
            _mm_store_si128(reinterpret_cast<__m128i*>(dst + 32), c);
            _mm_store_si128(reinterpret_cast<__m128i*>(dst + 48), d);
            dst += 64; src += 64; size -= 64;
        }
        while (size >= 16) {
            _mm_store_si128(reinterpret_cast<__m128i*>(dst), _mm_loadu_si128(reinterpret_cast<const __m128i*>(src)));
            dst += 16; src += 16; size -= 16;
        }
        if (size) std::memcpy(dst, src, size);
        return;
    }
#endif
    std::memcpy(destination, source, size);
}
} // namespace Common
