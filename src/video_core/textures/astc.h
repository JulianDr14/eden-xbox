// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Tegra::Texture::ASTC {

void Decompress(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth,
                uint32_t block_width, uint32_t block_height, std::span<uint8_t> output);

/// False when no block of data can decode to an alpha below 1: their color endpoint modes have no
/// alpha and their void-extent colors are opaque. Blocks are 16 bytes in any order (swizzled
/// guest memory is fine); reserved blocks, the zeroed padding of block-linear images, are opaque.
/// Endpoint modes with alpha make the whole data count as transparent, even if they encode 1.
[[nodiscard]] bool MayHaveAlpha(std::span<const uint8_t> blocks);

} // namespace Tegra::Texture::ASTC
