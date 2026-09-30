// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/textures/decoders.h"

TEST_CASE("Block-linear sector reads preserve texels and row tails", "[video_core][texture]") {
    using namespace Tegra::Texture;
    // SwizzleTexture retains its independent scalar implementation. Misaligned spans and guards
    // exercise SIMD loads/stores, short rows, sector/GOB edges, padded strides and 3D block order.
    for (const u32 bpp : {1U, 2U, 4U, 8U, 16U}) {
        for (const u32 width : {1U, 3U, 7U, 15U, 16U, 17U, 31U, 33U, 65U}) {
            for (const u32 height : {1U, 7U, 8U, 9U, 17U}) {
                for (const u32 depth : {1U, 3U}) {
                    for (const u32 block_height : {0U, 1U, 3U}) {
                        for (const u32 block_depth : {0U, 1U}) {
                            for (const u32 padding : {0U, 128U}) {
                                const u32 stride = ((width * bpp + 63) & ~63U) + padding;
                                const size_t size = size_t(width) * height * depth * bpp;
                                const size_t tiled_size = CalculateSize(
                                    true, 1, stride, height, depth, block_height, block_depth);
                                std::vector<u8> linear(size);
                                for (size_t i = 0; i < size; ++i)
                                    linear[i] = static_cast<u8>((i * 29 + i / 17 + 7) & 255);
                                std::vector<u8> tiled(tiled_size + 2, 0xCD);
                                std::vector<u8> output(size + 2, 0xAB);
                                const std::span<u8> tile_span{tiled.data() + 1, tiled_size};
                                SwizzleTexture(tile_span, linear, bpp, width, height, depth,
                                               block_height, block_depth, stride);
                                UnswizzleTexture({output.data() + 1, size}, tile_span, bpp,
                                                 width, height, depth, block_height, block_depth,
                                                 stride);
                                INFO("bpp=" << bpp << " size=" << width << 'x' << height << 'x'
                                            << depth << " blocks=" << block_height << '/'
                                            << block_depth << " stride=" << stride);
                                REQUIRE(std::equal(linear.begin(), linear.end(), output.begin() + 1));
                                REQUIRE(output.front() == 0xAB);
                                REQUIRE(output.back() == 0xAB);
                                REQUIRE(tiled.front() == 0xCD);
                                REQUIRE(tiled.back() == 0xCD);
                            }
                        }
                    }
                }
            }
        }
    }
}
