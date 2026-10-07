// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <numeric>
#include <utility>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/accelerated_swizzle.h"
#include "video_core/texture_cache/decode_bc.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"

// ASTC decoded on the GPU (and recompressed to BC3) on upload.

namespace D3D12 {

using namespace TextureDetail;

void TextureCacheRuntime::EnsureAstcRgbaScratch(u32 width, u32 height) {
    if (astc_rgba_scratch && astc_rgba_width >= width && astc_rgba_height >= height) {
        return;
    }
    if (astc_rgba_scratch) {
        scheduler.DeferRelease(std::move(astc_rgba_scratch));
    }
    astc_rgba_width = std::max(astc_rgba_width, width);
    astc_rgba_height = std::max(astc_rgba_height, height);
    // Growing both axes independently could retain a needlessly huge rectangle after unrelated
    // portrait and landscape uploads. Stay inside the promised workspace and fit this request.
    if (static_cast<u64>(astc_rgba_width) * astc_rgba_height * 4 >
        ASTC_RGBA_SCRATCH_BUDGET) {
        astc_rgba_width = width;
        astc_rgba_height = height;
    }
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = astc_rgba_width,
        .Height = astc_rgba_height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_TYPELESS,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(device.Get()->CreateCommittedResource(
                      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                      IID_PPV_ARGS(&astc_rgba_scratch)),
                  "Create ASTC RGBA scratch texture");
    astc_rgba_state = D3D12_RESOURCE_STATE_COMMON;
    LOG_INFO(Render, "D3D12: ASTC RGBA scratch {}x{} ({} KiB)", astc_rgba_width,
             astc_rgba_height,
             static_cast<u64>(astc_rgba_width) * astc_rgba_height * 4 / 1024);
}

void TextureCacheRuntime::EnsureAstcBc3Scratch(u64 size) {
    size = Common::AlignUp(size, static_cast<u64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    if (astc_bc3_scratch && astc_bc3_size >= size) {
        return;
    }
    if (astc_bc3_scratch) {
        scheduler.DeferRelease(std::move(astc_bc3_scratch));
    }
    astc_bc3_size = std::min(ASTC_BC3_SCRATCH_BUDGET,
                             std::bit_ceil(std::max<u64>(size, 64ULL * 1024)));
    astc_bc3_scratch = CreateTransferBuffer(device.Get(), astc_bc3_size, true);
    astc_bc3_state = D3D12_RESOURCE_STATE_COMMON;
    LOG_INFO(Render, "D3D12: ASTC BC3 scratch {} KiB", astc_bc3_size / 1024);
}

void TextureCacheRuntime::AccelerateImageUpload(
    Image& image, const StagingBufferRef& map,
    std::span<const VideoCommon::SwizzleParameters> swizzles, u32, u32) {
    if (!CanAccelerateImageUpload(image) || !image.Handle()) {
        return;
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploads, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploadBytes, image.guest_size_bytes);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureGpuDecodes, 1);
    if (!logged_gpu_astc) {
        LOG_INFO(Render, "D3D12: first ASTC image decoded on the GPU ({} {}x{}, {} layers, {} "
                 "levels)", image.info.format, image.info.size.width, image.info.size.height,
                 image.info.resources.layers, image.info.resources.levels);
        logged_gpu_astc = true;
    }
    const bool encode_bc3 = image.TransferFormat().copy_format == PixelFormat::BC3_UNORM;
    if (!encode_bc3) {
        image.Transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const u64 base = map.offset;
    static u32 logged_ranges = 0;
    if (logged_ranges < 16) {
        ++logged_ranges;
        const D3D12_RESOURCE_DESC buffer_desc = map.buffer->GetDesc();
        LOG_INFO(Render, "D3D12: ASTC GPU decode {} {}x{}x{} levels {}: blocks at 0x{:X}+0x{:X} "
                 "(buffer 0x{:X}, {} bytes), {} swizzles, layer stride {}",
                 image.info.format, image.info.size.width, image.info.size.height,
                 image.info.resources.layers, image.info.resources.levels,
                 map.buffer->GetGPUVirtualAddress() + base,
                 image.guest_size_bytes, map.buffer->GetGPUVirtualAddress(), buffer_desc.Width,
                 swizzles.size(), image.info.layer_stride);
    }
    const u32 layers = static_cast<u32>(image.info.resources.layers);
    for (const VideoCommon::SwizzleParameters& swizzle : swizzles) {
        const auto params = VideoCommon::Accelerated::MakeBlockLinearSwizzle2DParams(swizzle,
                                                                                     image.info);
        const u64 source_offset = base + swizzle.buffer_offset;
        const u64 buffer_size = map.buffer->GetDesc().Width;
        const u64 wanted_bytes = static_cast<u64>(params.layer_stride) * layers;
        const u64 source_bytes = source_offset < buffer_size
                                     ? std::min(wanted_bytes, buffer_size - source_offset)
                                     : 0;
        const u32 input_words = static_cast<u32>(std::min<u64>(source_bytes / 4,
                                                               std::numeric_limits<u32>::max()));
        const D3D12_CPU_DESCRIPTOR_HANDLE source_srv = view_descriptors.Allocate();
        const D3D12_SHADER_RESOURCE_VIEW_DESC source_desc{
            .Format = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {.FirstElement = input_words ? source_offset / 4 : 0,
                       .NumElements = input_words,
                       .StructureByteStride = 0,
                       .Flags = D3D12_BUFFER_SRV_FLAG_RAW},
        };
        device.Get()->CreateShaderResourceView(input_words ? map.buffer : nullptr, &source_desc,
                                               source_srv);

        const u32 block_width = VideoCore::Surface::DefaultBlockWidth(image.info.format);
        const u32 block_height = VideoCore::Surface::DefaultBlockHeight(image.info.format);
        if (!encode_bc3) {
            // Each level has a distinct subresource, so consecutive level dispatches require no
            // UAV barrier. The source is a bounded raw view rather than an unbounded root SRV.
            const D3D12_CPU_DESCRIPTOR_HANDLE uav = view_descriptors.Allocate();
            const D3D12_UNORDERED_ACCESS_VIEW_DESC desc{
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY,
                .Texture2DArray = {.MipSlice = static_cast<UINT>(swizzle.level),
                                   .FirstArraySlice = 0,
                                   .ArraySize = layers,
                                   .PlaneSlice = 0},
            };
            device.Get()->CreateUnorderedAccessView(image.Handle(), nullptr, &desc, uav);
            blit_helper->DecodeAstc({
                .source = source_srv,
                .destination = uav,
                .block_width = block_width,
                .block_height = block_height,
                .layer_stride = params.layer_stride,
                .block_size = params.block_size,
                .x_shift = params.x_shift,
                .gob_block_height = params.block_height,
                .gob_block_height_mask = params.block_height_mask,
                .blocks_x = swizzle.num_tiles.width,
                .blocks_y = swizzle.num_tiles.height,
                .layers = layers,
                .input_words = input_words,
                .first_block_row = 0,
            });
            view_descriptors.Free(uav);
            view_descriptors.Free(source_srv);
            continue;
        }

        const u32 mip_width = std::max(1U, image.info.size.width >> swizzle.level);
        const u32 mip_height = std::max(1U, image.info.size.height >> swizzle.level);
        const u32 bc3_row_pitch = AlignPitch(Common::DivCeil(mip_width, 4U) * 16U);
        const u32 band_alignment = std::lcm(block_height, 4U);
        const u32 rgba_rows = static_cast<u32>(std::max<u64>(
            band_alignment, ASTC_RGBA_SCRATCH_BUDGET / (static_cast<u64>(mip_width) * 4)));
        const u32 bc3_rows = static_cast<u32>(std::max<u64>(
            band_alignment, (ASTC_BC3_SCRATCH_BUDGET / bc3_row_pitch) * 4));
        u32 band_rows = std::min({mip_height, rgba_rows, bc3_rows});
        band_rows = std::max(band_alignment, band_rows / band_alignment * band_alignment);
        band_rows = std::min(band_rows, Common::AlignUp(mip_height, band_alignment));
        if (gpu_astc_fresh.load(std::memory_order_relaxed)) {
            if (astc_rgba_scratch) {
                scheduler.DeferRelease(std::move(astc_rgba_scratch));
            }
            if (astc_bc3_scratch) {
                scheduler.DeferRelease(std::move(astc_bc3_scratch));
            }
            astc_rgba_width = astc_rgba_height = 0;
            astc_bc3_size = 0;
        }
        EnsureAstcRgbaScratch(mip_width, band_rows);
        const u64 bc3_bytes = static_cast<u64>(bc3_row_pitch) * Common::DivCeil(band_rows, 4U);
        EnsureAstcBc3Scratch(bc3_bytes);

        for (u32 first_y = 0; first_y < mip_height; first_y += band_rows) {
            const u32 rows = std::min(band_rows, mip_height - first_y);
            auto* const commands = scheduler.CommandList();
            if (astc_rgba_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                TransitionBuffer(commands, astc_rgba_scratch.Get(), astc_rgba_state,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                astc_rgba_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            } else {
                const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                                     .UAV = {.pResource = astc_rgba_scratch.Get()}};
                commands->ResourceBarrier(1, &barrier);
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE rgba_uav = view_descriptors.Allocate();
            const D3D12_UNORDERED_ACCESS_VIEW_DESC rgba_uav_desc{
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY,
                .Texture2DArray = {.MipSlice = 0, .FirstArraySlice = 0, .ArraySize = 1,
                                   .PlaneSlice = 0},
            };
            device.Get()->CreateUnorderedAccessView(astc_rgba_scratch.Get(), nullptr,
                                                    &rgba_uav_desc, rgba_uav);
            blit_helper->DecodeAstc({
                .source = source_srv,
                .destination = rgba_uav,
                .block_width = block_width,
                .block_height = block_height,
                .layer_stride = params.layer_stride,
                .block_size = params.block_size,
                .x_shift = params.x_shift,
                .gob_block_height = params.block_height,
                .gob_block_height_mask = params.block_height_mask,
                .blocks_x = swizzle.num_tiles.width,
                .blocks_y = Common::DivCeil(rows, block_height),
                .layers = 1,
                .input_words = input_words,
                .first_block_row = first_y / block_height,
            });
            view_descriptors.Free(rgba_uav);

            TransitionBuffer(commands, astc_rgba_scratch.Get(), astc_rgba_state,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            astc_rgba_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (astc_bc3_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                TransitionBuffer(commands, astc_bc3_scratch.Get(), astc_bc3_state,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                astc_bc3_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE rgba_srv = view_descriptors.Allocate();
            const D3D12_SHADER_RESOURCE_VIEW_DESC rgba_srv_desc{
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1,
                              .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f},
            };
            device.Get()->CreateShaderResourceView(astc_rgba_scratch.Get(), &rgba_srv_desc,
                                                   rgba_srv);
            blit_helper->EncodeBc3({rgba_srv, astc_bc3_scratch->GetGPUVirtualAddress(),
                                    mip_width, rows, rows,
                                    bc3_row_pitch / 4});
            view_descriptors.Free(rgba_srv);

            TransitionBuffer(commands, astc_bc3_scratch.Get(), astc_bc3_state,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            astc_bc3_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            if (first_y == 0 && gpu_astc_verify.fetch_sub(1, std::memory_order_relaxed) > 0) {
                // Deliberately synchronous and opt-in: compare the actual GPU bytes with the
                // established CPU ASTC->BC3 path before the destination texture can hide whether
                // a mismatch came from encoding or CopyTextureRegion.
                StagingBufferRef readback = DownloadStagingBuffer(bc3_bytes, true);
                commands->CopyBufferRegion(readback.buffer, readback.offset,
                                           astc_bc3_scratch.Get(), 0, bc3_bytes);
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
            image.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
            const D3D12_TEXTURE_COPY_LOCATION src{
                .pResource = astc_bc3_scratch.Get(),
                .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                .PlacedFootprint = {
                    .Offset = 0,
                    .Footprint = {.Format = image.FootprintFormat(),
                                  .Width = Common::AlignUp(mip_width, 4U),
                                  .Height = Common::AlignUp(rows, 4U),
                                  .Depth = 1,
                                  .RowPitch = bc3_row_pitch},
                },
            };
            const D3D12_TEXTURE_COPY_LOCATION dst{
                .pResource = image.Handle(),
                .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = image.Subresource(swizzle.level, 0),
            };
            commands->CopyTextureRegion(&dst, 0, first_y, 0, &src, nullptr);
        }
        view_descriptors.Free(source_srv);
    }
    if (gpu_astc_sync.load(std::memory_order_relaxed)) {
        scheduler.Finish();
    }
}

} // namespace D3D12
