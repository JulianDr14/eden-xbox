// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace VideoCommon::Bc45 {

// Match the existing SwiftShader-derived decoder's integer interpolation, including
// its signed endpoint handling. No new quantization or GPU format policy is introduced.
template <bool Signed>
inline std::array<std::uint8_t, 8> Palette(const std::uint8_t* block) {
    const auto Endpoint = [](std::uint8_t value) -> int {
        if constexpr (Signed) return value < 128 ? value : static_cast<int>(value) - 256;
        return value;
    };
    const int first = Endpoint(block[0]);
    const int second = Endpoint(block[1]);
    std::array<std::uint8_t, 8> result{block[0], block[1]};
    if (first > second) {
        for (int i = 2; i < 8; ++i) {
            result[i] = static_cast<std::uint8_t>(((8 - i) * first + (i - 1) * second) / 7);
        }
    } else {
        for (int i = 2; i < 6; ++i) {
            result[i] = static_cast<std::uint8_t>(((6 - i) * first + (i - 1) * second) / 5);
        }
        result[6] = Signed ? 128 : 0;
        result[7] = Signed ? 127 : 255;
    }
    return result;
}

// Only full 4x4 blocks. One packed store per row; BC5 channels are interleaved
// before writing instead of walking the destination twice. memcpy permits unaligned
// buffers and avoids strict-aliasing violations. No heap allocation or ISA dependency.
template <unsigned Channels, bool Signed>
inline void DecodeBlock(const std::uint8_t* source, std::uint8_t* destination,
                        std::size_t pitch) {
    static_assert(Channels == 1 || Channels == 2);
    static_assert(std::endian::native == std::endian::little);
    using Row = std::conditional_t<Channels == 1, std::uint32_t, std::uint64_t>;
    std::array<std::array<std::uint8_t, 8>, Channels> palette;
    std::array<std::uint64_t, Channels> indices;
    for (unsigned channel = 0; channel < Channels; ++channel) {
        const auto* block = source + channel * 8;
        palette[channel] = Palette<Signed>(block);
        std::uint64_t word;
        std::memcpy(&word, block, sizeof(word));
        indices[channel] = word >> 16;
    }
    for (unsigned y = 0; y < 4; ++y) {
        Row row{};
        for (unsigned x = 0; x < 4; ++x) {
            for (unsigned channel = 0; channel < Channels; ++channel) {
                row |= static_cast<Row>(palette[channel][indices[channel] & 7]) <<
                       ((x * Channels + channel) * 8);
                indices[channel] >>= 3;
            }
        }
        std::memcpy(destination + y * pitch, &row, sizeof(row));
    }
}

// Caller selects this only when width/height and source pitch have full BC blocks.
template <unsigned Channels, bool Signed>
inline void DecodeFullBlocks(const std::uint8_t* source, std::uint8_t* destination,
                             std::size_t width, std::size_t height,
                             std::size_t source_row_texels) {
    const std::size_t source_pitch = source_row_texels / 4 * Channels * 8;
    for (std::size_t y = 0; y < height; y += 4) {
        for (std::size_t x = 0; x < width; x += 4) {
            DecodeBlock<Channels, Signed>(source + y / 4 * source_pitch + x / 4 * Channels * 8,
                                         destination + (y * width + x) * Channels, width * Channels);
        }
    }
}

} // namespace VideoCommon::Bc45
