// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SetAstcGpuVerify: the first bands the GPU decodes and re-encodes are read back and compared with
// the CPU ASTC -> BC3 path. Deliberately synchronous; diagnostics only.

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"
#include "video_core/texture_cache/decode_bc.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"

namespace D3D12 {

using namespace TextureDetail;

void TextureCacheRuntime::VerifyGpuAstcBand(const AstcVerifyBand& band) {
    // Compare the actual GPU bytes with the established CPU ASTC->BC3 path before the
    // destination texture can hide whether a mismatch came from encoding or CopyTextureRegion.
    const Image& image = band.image;
    const StagingBufferRef& map = band.map;
    const VideoCommon::SwizzleParameters& swizzle = band.swizzle;
    const auto& params = band.params;
    const u32 mip_width = band.mip_width;
    const u32 rows = band.rows;
    const u32 block_width = band.block_width;
    const u32 block_height = band.block_height;
    const u32 bc3_row_pitch = band.bc3_row_pitch;
    const u64 bc3_bytes = band.bc3_bytes;
    auto* const commands = scheduler.CommandList();
    StagingBufferRef readback = DownloadStagingBuffer(bc3_bytes, true);
    commands->CopyBufferRegion(readback.buffer, readback.offset,
                               astc_bc_scratch.Get(), 0, bc3_bytes);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT rgba_footprint{};
    u64 rgba_readback_size = 0;
    const D3D12_RESOURCE_DESC rgba_desc = astc_rgba_scratch->GetDesc();
    device.Get()->GetCopyableFootprints(&rgba_desc, 0, 1, 0, &rgba_footprint, nullptr,
                                         nullptr, &rgba_readback_size);
    StagingBufferRef rgba_readback = DownloadStagingBuffer(rgba_readback_size, true);
    TransitionBuffer(commands, astc_rgba_scratch.Get(), astc_rgba_state,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
    astc_rgba_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rgba_footprint.Offset = rgba_readback.offset;
    const D3D12_TEXTURE_COPY_LOCATION rgba_source{
        .pResource = astc_rgba_scratch.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0};
    const D3D12_TEXTURE_COPY_LOCATION rgba_destination{
        .pResource = rgba_readback.buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = rgba_footprint};
    commands->CopyTextureRegion(&rgba_destination, 0, 0, 0, &rgba_source, nullptr);
    scheduler.Finish();

    const u32 blocks_x = Common::DivCeil(mip_width, block_width);
    const u32 blocks_y = Common::DivCeil(rows, block_height);
    std::vector<u8> linear_astc(static_cast<size_t>(blocks_x) * blocks_y * 16);
    bool source_valid = true;
    u64 largest_source = 0;
    u64 first_invalid_source = 0;
    for (u32 block_y = 0; block_y < blocks_y; ++block_y) {
        for (u32 block_x = 0; block_x < blocks_x; ++block_x) {
            const u32 byte_x = block_x << 4U;
            const u32 gob_y = block_y >> 3U;
            const u32 swizzled = ((byte_x & 32U) << 3U) |
                                  ((block_y & 6U) << 5U) |
                                  ((byte_x & 16U) << 1U) |
                                  ((block_y & 1U) << 4U) | (byte_x & 15U);
            u64 offset = (gob_y >> params.block_height) * params.block_size;
            offset += (gob_y & params.block_height_mask) << 9U;
            offset += (byte_x >> 6U) << params.x_shift;
            offset += swizzled;
            const u64 source = swizzle.buffer_offset + offset;
            largest_source = std::max(largest_source, source + 16);
            const size_t destination =
                (static_cast<size_t>(block_y) * blocks_x + block_x) * 16;
            if (source + 16 > map.mapped_span.size()) {
                if (source_valid) {
                    first_invalid_source = source;
                }
                source_valid = false;
                continue;
            }
            std::memcpy(linear_astc.data() + destination,
                        map.mapped_span.data() + source, 16);
        }
    }
    std::vector<u8> rgba(static_cast<size_t>(mip_width) * rows * 4);
    std::vector<u8> expected(static_cast<size_t>(Common::DivCeil(mip_width, 4U)) *
                             Common::DivCeil(rows, 4U) * 16);
    Tegra::Texture::ASTC::Decompress(linear_astc, mip_width, rows, 1, block_width,
                                     block_height, rgba);
    Tegra::Texture::BCN::CompressBC3(rgba, mip_width, rows, 1, expected);
    const u32 tight_row = Common::DivCeil(mip_width, 4U) * 16;
    std::vector<u8> gpu_bc3(expected.size());
    for (u32 row = 0; row < Common::DivCeil(rows, 4U); ++row) {
        std::memcpy(gpu_bc3.data() + static_cast<size_t>(row) * tight_row,
                    readback.mapped_span.data() + static_cast<size_t>(row) *
                                                      bc3_row_pitch,
                    tight_row);
    }
    std::vector<u8> gpu_rgba(rgba.size());
    VideoCommon::BufferImageCopy verify_copy{
        .buffer_offset = 0,
        .buffer_size = gpu_bc3.size(),
        .buffer_row_length = Common::AlignUp(mip_width, 4U),
        .buffer_image_height = Common::AlignUp(rows, 4U),
        .image_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
        .image_offset = {},
        .image_extent = {mip_width, rows, 1},
    };
    VideoCommon::DecompressBCn(gpu_bc3, gpu_rgba, verify_copy,
                               PixelFormat::BC3_UNORM);
    std::array<u64, 4> absolute_error{};
    std::array<u64, 4> large_error{};
    u64 lost_alpha = 0;
    for (size_t pixel = 0; pixel < rgba.size() / 4; ++pixel) {
        for (size_t channel = 0; channel < 4; ++channel) {
            const u32 error = static_cast<u32>(std::abs(
                static_cast<int>(gpu_rgba[pixel * 4 + channel]) -
                static_cast<int>(rgba[pixel * 4 + channel])));
            absolute_error[channel] += error;
            large_error[channel] += error > 32;
        }
        lost_alpha += rgba[pixel * 4 + 3] >= 128 && gpu_rgba[pixel * 4 + 3] < 32;
    }
    u64 mismatches = 0;
    u64 rgba_mismatches = 0;
    size_t first_mismatch = std::numeric_limits<size_t>::max();
    size_t first_rgba_mismatch = std::numeric_limits<size_t>::max();
    for (u32 row = 0; row < rows; ++row) {
        for (u32 byte = 0; byte < mip_width * 4; ++byte) {
            const size_t tight = static_cast<size_t>(row) * mip_width * 4 + byte;
            const u8 gpu = rgba_readback.mapped_span[
                rgba_footprint.Offset - rgba_readback.offset +
                static_cast<size_t>(row) * rgba_footprint.Footprint.RowPitch + byte];
            if (gpu != rgba[tight]) {
                ++rgba_mismatches;
                first_rgba_mismatch = std::min(first_rgba_mismatch, tight);
            }
        }
    }
    for (u32 row = 0; row < Common::DivCeil(rows, 4U); ++row) {
        for (u32 byte = 0; byte < tight_row; ++byte) {
            const u8 gpu = readback.mapped_span[static_cast<size_t>(row) *
                                                    bc3_row_pitch + byte];
            const u8 cpu = expected[static_cast<size_t>(row) * tight_row + byte];
            if (gpu != cpu) {
                ++mismatches;
                first_mismatch = std::min(first_mismatch,
                                          static_cast<size_t>(row) * tight_row + byte);
            }
        }
    }
    if (first_rgba_mismatch != std::numeric_limits<size_t>::max()) {
        const size_t pixel = first_rgba_mismatch / 4;
        const u32 px = static_cast<u32>(pixel % mip_width);
        const u32 py = static_cast<u32>(pixel / mip_width);
        const u8* const gpu_px = rgba_readback.mapped_span.data() +
                                 (rgba_footprint.Offset - rgba_readback.offset) +
                                 static_cast<size_t>(py) *
                                     rgba_footprint.Footprint.RowPitch +
                                 static_cast<size_t>(px) * 4;
        const u8* const cpu_px = rgba.data() + pixel * 4;
        u64 bad_blocks = 0;
        const u32 blocks_x_check = Common::DivCeil(mip_width, block_width);
        for (u32 by = 0; by < Common::DivCeil(rows, block_height); ++by) {
            for (u32 bx = 0; bx < blocks_x_check; ++bx) {
                bool bad = false;
                for (u32 y = by * block_height;
                     !bad && y < std::min(rows, (by + 1) * block_height); ++y) {
                    for (u32 x = bx * block_width;
                         !bad && x < std::min(mip_width, (bx + 1) * block_width);
                         ++x) {
                        const u8* g = rgba_readback.mapped_span.data() +
                                      (rgba_footprint.Offset - rgba_readback.offset) +
                                      static_cast<size_t>(y) *
                                          rgba_footprint.Footprint.RowPitch +
                                      static_cast<size_t>(x) * 4;
                        const u8* c = rgba.data() +
                                      (static_cast<size_t>(y) * mip_width + x) * 4;
                        bad = std::memcmp(g, c, 4) != 0;
                    }
                }
                bad_blocks += bad;
            }
        }
        const size_t block_offset =
            (static_cast<size_t>(py / block_height) * blocks_x_check +
             px / block_width) * 16;
        const u8* const blk = linear_astc.data() + block_offset;
        LOG_INFO(Render,
                 "D3D12: ASTC verify {} {}x{}: {} of {} blocks differ; first at "
                 "({}, {}) GPU {:02X}{:02X}{:02X}{:02X} CPU {:02X}{:02X}{:02X}{:02X}; "
                 "block {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}"
                 "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                 image.info.format, mip_width, rows, bad_blocks,
                 blocks_x_check * Common::DivCeil(rows, block_height), px, py,
                 gpu_px[0], gpu_px[1], gpu_px[2], gpu_px[3], cpu_px[0], cpu_px[1],
                 cpu_px[2], cpu_px[3], blk[0], blk[1], blk[2], blk[3], blk[4],
                 blk[5], blk[6], blk[7], blk[8], blk[9], blk[10], blk[11], blk[12],
                 blk[13], blk[14], blk[15]);
    }
    LOG_INFO(Render,
             "D3D12: ASTC verify {}x{} source {}: RGBA differs in {} / {} bytes first "
             "{}; GPU BC3 differs in {} / {} bytes first {}; source span {} max {} "
             "invalid {}", mip_width, rows,
             source_valid ? "valid" : "INVALID",
             rgba_mismatches, rgba.size(),
             first_rgba_mismatch == std::numeric_limits<size_t>::max()
                 ? 0
                 : first_rgba_mismatch,
             mismatches, expected.size(),
             first_mismatch == std::numeric_limits<size_t>::max() ? 0
                                                                 : first_mismatch,
             map.mapped_span.size(), largest_source, first_invalid_source);
    LOG_INFO(Render,
             "D3D12: ASTC verify decoded BC3 abs error RGBA [{}, {}, {}, {}], >32 "
             "[{}, {}, {}, {}], lost alpha {} / {} pixels",
             absolute_error[0], absolute_error[1], absolute_error[2],
             absolute_error[3], large_error[0], large_error[1], large_error[2],
             large_error[3], lost_alpha, rgba.size() / 4);
    LOG_INFO(Render,
             "D3D12: ASTC verify block0 GPU {:02X} {:02X} {:02X} {:02X} {:02X} "
             "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
             "{:02X} {:02X}; CPU {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
             "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
             gpu_bc3[0], gpu_bc3[1], gpu_bc3[2], gpu_bc3[3], gpu_bc3[4], gpu_bc3[5],
             gpu_bc3[6], gpu_bc3[7], gpu_bc3[8], gpu_bc3[9], gpu_bc3[10], gpu_bc3[11],
             gpu_bc3[12], gpu_bc3[13], gpu_bc3[14], gpu_bc3[15], expected[0],
             expected[1], expected[2], expected[3], expected[4], expected[5],
             expected[6], expected[7], expected[8], expected[9], expected[10],
             expected[11], expected[12], expected[13], expected[14], expected[15]);
    FreeDeferredStagingBuffer(readback);
    FreeDeferredStagingBuffer(rgba_readback);
}

} // namespace D3D12
