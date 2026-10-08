// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <atomic>
#include <bit>
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

// ASTC decoded on the GPU (and recompressed to BC1 or BC3) on upload.

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

void TextureCacheRuntime::EnsureAstcBcScratch(u64 size) {
    size = Common::AlignUp(size, static_cast<u64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    if (astc_bc_scratch && astc_bc_size >= size) {
        return;
    }
    if (astc_bc_scratch) {
        scheduler.DeferRelease(std::move(astc_bc_scratch));
    }
    astc_bc_size = std::min(ASTC_BC_SCRATCH_BUDGET,
                             std::bit_ceil(std::max<u64>(size, 64ULL * 1024)));
    astc_bc_scratch = CreateTransferBuffer(device.Get(), astc_bc_size, true);
    astc_bc_state = D3D12_RESOURCE_STATE_COMMON;
    LOG_INFO(Render, "D3D12: ASTC BC scratch {} KiB", astc_bc_size / 1024);
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
    const PixelFormat copy_format = image.TransferFormat().copy_format;
    const bool encode_bc1 = copy_format == PixelFormat::BC1_RGBA_UNORM;
    const bool encode_bc = encode_bc1 || copy_format == PixelFormat::BC3_UNORM;
    if (!encode_bc) {
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
        if (!encode_bc) {
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
        const u32 bc_row_pitch =
            AlignPitch(Common::DivCeil(mip_width, 4U) * (encode_bc1 ? 8U : 16U));
        const u32 band_alignment = std::lcm(block_height, 4U);
        const u32 rgba_rows = static_cast<u32>(std::max<u64>(
            band_alignment, ASTC_RGBA_SCRATCH_BUDGET / (static_cast<u64>(mip_width) * 4)));
        const u32 bc_rows = static_cast<u32>(std::max<u64>(
            band_alignment, (ASTC_BC_SCRATCH_BUDGET / bc_row_pitch) * 4));
        u32 band_rows = std::min({mip_height, rgba_rows, bc_rows});
        band_rows = std::max(band_alignment, band_rows / band_alignment * band_alignment);
        band_rows = std::min(band_rows, Common::AlignUp(mip_height, band_alignment));
        if (gpu_astc_fresh.load(std::memory_order_relaxed)) {
            if (astc_rgba_scratch) {
                scheduler.DeferRelease(std::move(astc_rgba_scratch));
            }
            if (astc_bc_scratch) {
                scheduler.DeferRelease(std::move(astc_bc_scratch));
            }
            astc_rgba_width = astc_rgba_height = 0;
            astc_bc_size = 0;
        }
        EnsureAstcRgbaScratch(mip_width, band_rows);
        const u64 bc_bytes = static_cast<u64>(bc_row_pitch) * Common::DivCeil(band_rows, 4U);
        EnsureAstcBcScratch(bc_bytes);

        // One layer at a time through the same bounded scratch: each layer's blocks start
        // layer_stride bytes after the previous one's, and each is its own subresource.
        for (u32 layer = 0; layer < layers; ++layer) {
            const u64 layer_offset = static_cast<u64>(params.layer_stride) * layer;
            const D3D12_CPU_DESCRIPTOR_HANDLE layer_srv =
                layer == 0 ? source_srv : view_descriptors.Allocate();
            if (layer != 0) {
                const u64 layer_source = source_offset + layer_offset;
                const u64 layer_bytes =
                    layer_source < buffer_size
                        ? std::min<u64>(params.layer_stride, buffer_size - layer_source)
                        : 0;
                const u32 layer_words = static_cast<u32>(layer_bytes / 4);
                const D3D12_SHADER_RESOURCE_VIEW_DESC layer_desc{
                    .Format = DXGI_FORMAT_R32_TYPELESS,
                    .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                    .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                    .Buffer = {.FirstElement = layer_words ? layer_source / 4 : 0,
                               .NumElements = layer_words,
                               .StructureByteStride = 0,
                               .Flags = D3D12_BUFFER_SRV_FLAG_RAW},
                };
                device.Get()->CreateShaderResourceView(layer_words ? map.buffer : nullptr,
                                                       &layer_desc, layer_srv);
            }
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
                    .source = layer_srv,
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
                if (astc_bc_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                    TransitionBuffer(commands, astc_bc_scratch.Get(), astc_bc_state,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    astc_bc_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
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
                blit_helper->EncodeBc({rgba_srv, astc_bc_scratch->GetGPUVirtualAddress(),
                                       mip_width, rows, rows, bc_row_pitch / 4, encode_bc1});
                view_descriptors.Free(rgba_srv);

                TransitionBuffer(commands, astc_bc_scratch.Get(), astc_bc_state,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
                astc_bc_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
                // The check compares with the CPU's BC3.
                if (!encode_bc1 && first_y == 0 && layer == 0 &&
                    gpu_astc_verify.fetch_sub(1, std::memory_order_relaxed) > 0) {
                    VerifyGpuAstcBand({.image = image, .map = map, .swizzle = swizzle,
                                       .params = params, .mip_width = mip_width, .rows = rows,
                                       .block_width = block_width, .block_height = block_height,
                                       .bc3_row_pitch = bc_row_pitch, .bc3_bytes = bc_bytes});
                }
                image.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
                const D3D12_TEXTURE_COPY_LOCATION src{
                    .pResource = astc_bc_scratch.Get(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    .PlacedFootprint = {
                        .Offset = 0,
                        .Footprint = {.Format = image.FootprintFormat(),
                                      .Width = Common::AlignUp(mip_width, 4U),
                                      .Height = Common::AlignUp(rows, 4U),
                                      .Depth = 1,
                                      .RowPitch = bc_row_pitch},
                    },
                };
                const D3D12_TEXTURE_COPY_LOCATION dst{
                    .pResource = image.Handle(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = image.Subresource(swizzle.level, layer),
                };
                commands->CopyTextureRegion(&dst, 0, first_y, 0, &src, nullptr);
            }
            if (layer != 0) {
                view_descriptors.Free(layer_srv);
            }
        }
        view_descriptors.Free(source_srv);
    }
    if (gpu_astc_sync.load(std::memory_order_relaxed)) {
        scheduler.Finish();
    }
}

} // namespace D3D12
