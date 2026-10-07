// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <utility>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"

// Depth-stencil images split into and merged from their planes (D3D12 copies one plane at a
// time; the guest packs both together).

namespace D3D12 {

using namespace TextureDetail;

Image::PlaneFootprints Image::Footprints(s32 level) const {
    // Both planes of one subresource in one buffer: plane 1 starts at the next placement
    // boundary after plane 0. The footprint of a level does not depend on its layer.
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    PlaneFootprints result{};
    u64 depth_bytes = 0;
    runtime->device.Get()->GetCopyableFootprints(&desc, Subresource(level, 0, 0), 1, 0,
                                                 &result.planes[0], nullptr, nullptr,
                                                 &depth_bytes);
    const u64 stencil_offset = Common::AlignUp(depth_bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    u64 stencil_bytes = 0;
    runtime->device.Get()->GetCopyableFootprints(&desc, Subresource(level, 0, 1), 1,
                                                 stencil_offset, &result.planes[1], nullptr,
                                                 nullptr, &stencil_bytes);
    result.size = stencil_offset + stencil_bytes;
    return result;
}

void Image::CopyPlanes(s32 level, s32 layer, ID3D12Resource* buffer,
                       const PlaneFootprints& footprints, bool to_buffer) {
    Transition(to_buffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
    auto* const commands = runtime->scheduler.CommandList();
    for (u32 plane = 0; plane < 2; ++plane) {
        const D3D12_TEXTURE_COPY_LOCATION texture{
            .pResource = resource.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = Subresource(level, layer, plane)};
        const D3D12_TEXTURE_COPY_LOCATION placed{.pResource = buffer,
                                                 .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                 .PlacedFootprint = footprints.planes[plane]};
        // Whole subresources only: CopyTextureRegion does not copy part of a depth-stencil one
        // (offsets 0 and no source box).
        if (to_buffer) {
            commands->CopyTextureRegion(&placed, 0, 0, 0, &texture, nullptr);
        } else {
            commands->CopyTextureRegion(&texture, 0, 0, 0, &placed, nullptr);
        }
    }
}

void Image::UploadDepthStencil(ID3D12Resource* buffer, size_t base_offset,
                               std::span<const BufferImageCopy> copies) {
    BlitImageHelper* const helper = runtime->blit_helper;
    const DepthStencilLayout guest_layout = *GuestDepthStencilLayout(info.format);
    ID3D12Device* const device = runtime->device.Get();
    // The pack shader reads the staging memory directly: UPLOAD buffers already are readable,
    // the buffer cache's DEFAULT ones rest in COMMON.
    const bool source_default = HeapType(buffer) == D3D12_HEAP_TYPE_DEFAULT;
    if (source_default) {
        TransitionBuffer(runtime->scheduler.CommandList(), buffer, D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    const D3D12_GPU_VIRTUAL_ADDRESS source_base = buffer->GetGPUVirtualAddress();
    for (const auto& copy : copies) {
        const CopyLayout layout = Layout(copy);
        const s32 level = copy.image_subresource.base_level;
        const PlaneFootprints footprints = Footprints(level);
        const u32 level_width = footprints.planes[0].Footprint.Width;
        const u32 level_height = footprints.planes[0].Footprint.Height;
        const u32 x = std::min(static_cast<u32>(copy.image_offset.x), level_width);
        const u32 y = std::min(static_cast<u32>(copy.image_offset.y), level_height);
        const u32 width = std::min(copy.image_extent.width, level_width - x);
        const u32 height = std::min(copy.image_extent.height, level_height - y);
        const bool whole = x == 0 && y == 0 && width == level_width && height == level_height;
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
        for (u32 layer = 0; layer < layers; ++layer) {
            const s32 image_layer = copy.image_subresource.base_layer + static_cast<s32>(layer);
            const u64 source_offset = base_offset + copy.buffer_offset +
                                      static_cast<u64>(layer) * layout.tight_slice;
            if (source_offset % sizeof(u32) != 0 || layout.row_bytes % sizeof(u32) != 0) {
                WarnOnce(logged_depth_stencil_unaligned, "depth-stencil ({}) upload at offset {} "
                         "is not word aligned; skipped", info.format, source_offset);
                continue;
            }
            auto* const commands = runtime->scheduler.CommandList();
            ComPtr<ID3D12Resource> planes =
                runtime->transfer_buffers.Acquire(footprints.size, true);
            if (whole) {
                TransitionBuffer(commands, planes.Get(), D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            } else {
                // The planes go back as whole subresources: start from what the image holds so
                // the texels outside the region stay.
                CopyPlanes(level, image_layer, planes.Get(), footprints, true);
                TransitionBuffer(commands, planes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            const D3D12_GPU_VIRTUAL_ADDRESS address = source_base + source_offset;
            const D3D12_GPU_VIRTUAL_ADDRESS aligned = address & ~D3D12_GPU_VIRTUAL_ADDRESS{15};
            helper->SplitDepthStencil({
                .source = aligned,
                .destination = planes->GetGPUVirtualAddress(),
                .packed_offset = static_cast<u32>((address - aligned) / sizeof(u32)),
                .packed_row = layout.row_bytes / static_cast<u32>(sizeof(u32)),
                .depth_pitch = footprints.planes[0].Footprint.RowPitch / 4,
                .stencil_offset = static_cast<u32>(footprints.planes[1].Offset / 4),
                .stencil_pitch = footprints.planes[1].Footprint.RowPitch / 4,
                .x = x,
                .y = y,
                .width = width,
                .height = height,
                .row_texels = 0,
                .layout = guest_layout,
            });
            TransitionBuffer(commands, planes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            CopyPlanes(level, image_layer, planes.Get(), footprints, false);
            runtime->transfer_buffers.Release(std::move(planes));
            if (!logged_depth_stencil_upload) {
                logged_depth_stencil_upload = true;
                LOG_INFO(Render, "D3D12: first depth-stencil upload ({} {}x{} level {} region "
                         "{},{} {}x{}, {} source) split into its planes",
                         info.format, level_width, level_height, level, x, y, width, height,
                         source_default ? "GPU" : "staging");
            }
        }
    }
    if (source_default) {
        TransitionBuffer(runtime->scheduler.CommandList(), buffer,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_COMMON);
    }
}

void Image::DownloadDepthStencil(std::span<ID3D12Resource*> buffers, std::span<size_t> offsets,
                                 std::span<const BufferImageCopy> copies) {
    BlitImageHelper* const helper = runtime->blit_helper;
    const DepthStencilLayout guest_layout = *GuestDepthStencilLayout(info.format);
    const u32 texel_bytes = VideoCore::Surface::BytesPerBlock(format.copy_format);
    ID3D12Device* const device = runtime->device.Get();
    bool copied = false;
    for (const auto& copy : copies) {
        const CopyLayout layout = Layout(copy);
        const s32 level = copy.image_subresource.base_level;
        const PlaneFootprints footprints = Footprints(level);
        const u32 level_width = footprints.planes[0].Footprint.Width;
        const u32 level_height = footprints.planes[0].Footprint.Height;
        const u32 x = std::min(static_cast<u32>(copy.image_offset.x), level_width);
        const u32 y = std::min(static_cast<u32>(copy.image_offset.y), level_height);
        const u32 width = std::min(copy.image_extent.width, level_width - x);
        const u32 height = std::min(copy.image_extent.height, level_height - y);
        const u64 packed_size = static_cast<u64>(layout.row_bytes) * height;
        if (packed_size == 0 || layout.row_bytes % sizeof(u32) != 0) {
            continue;
        }
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
        for (u32 layer = 0; layer < layers; ++layer) {
            const s32 image_layer = copy.image_subresource.base_layer + static_cast<s32>(layer);
            auto* const commands = runtime->scheduler.CommandList();
            ComPtr<ID3D12Resource> planes = runtime->transfer_buffers.Acquire(footprints.size);
            CopyPlanes(level, image_layer, planes.Get(), footprints, true);
            TransitionBuffer(commands, planes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> packed = runtime->transfer_buffers.Acquire(packed_size, true);
            TransitionBuffer(commands, packed.Get(), D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            helper->MergeDepthStencil({
                .source = planes->GetGPUVirtualAddress(),
                .destination = packed->GetGPUVirtualAddress(),
                .packed_offset = 0,
                .packed_row = layout.row_bytes / static_cast<u32>(sizeof(u32)),
                .depth_pitch = footprints.planes[0].Footprint.RowPitch / 4,
                .stencil_offset = static_cast<u32>(footprints.planes[1].Offset / 4),
                .stencil_pitch = footprints.planes[1].Footprint.RowPitch / 4,
                .x = x,
                .y = y,
                .width = width,
                .height = height,
                .row_texels = layout.row_bytes / texel_bytes,
                .layout = guest_layout,
            });
            TransitionBuffer(commands, packed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            // As in the color path: every buffer receives every copy, each at its own offset.
            for (size_t target = 0; target < buffers.size(); ++target) {
                const u64 dest_offset = offsets[target] + copy.buffer_offset +
                                        static_cast<u64>(layer) * layout.tight_slice;
                commands->CopyBufferRegion(buffers[target], dest_offset, packed.Get(), 0,
                                           packed_size);
            }
            copied = true;
            runtime->transfer_buffers.Release(std::move(planes));
            runtime->transfer_buffers.Release(std::move(packed));
            if (!logged_depth_stencil_download) {
                logged_depth_stencil_download = true;
                LOG_INFO(Render, "D3D12: first depth-stencil download ({} {}x{} level {} region "
                         "{},{} {}x{}) merged from its planes",
                         info.format, level_width, level_height, level, x, y, width, height);
            }
        }
    }
    if (copied) {
        for (ID3D12Resource* const buffer : buffers) {
            DecayIfDefault(runtime->scheduler.CommandList(), buffer,
                           D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
}

void Image::CopyDepthStencilFrom(Image& src, std::span<const ImageCopy> copies) {
    ID3D12Device* const device = runtime->device.Get();
    for (const auto& copy : copies) {
        const PlaneFootprints src_footprints = src.Footprints(copy.src_subresource.base_level);
        const PlaneFootprints dst_footprints = Footprints(copy.dst_subresource.base_level);
        const auto& src_size = src_footprints.planes[0].Footprint;
        const auto& dst_size = dst_footprints.planes[0].Footprint;
        const u32 src_x = std::min(static_cast<u32>(copy.src_offset.x), src_size.Width);
        const u32 src_y = std::min(static_cast<u32>(copy.src_offset.y), src_size.Height);
        const u32 dst_x = std::min(static_cast<u32>(copy.dst_offset.x), dst_size.Width);
        const u32 dst_y = std::min(static_cast<u32>(copy.dst_offset.y), dst_size.Height);
        const u32 width = std::min({copy.extent.width, src_size.Width - src_x,
                                    dst_size.Width - dst_x});
        const u32 height = std::min({copy.extent.height, src_size.Height - src_y,
                                     dst_size.Height - dst_y});
        if (width == 0 || height == 0) {
            continue;
        }
        const bool whole = src_x == 0 && src_y == 0 && dst_x == 0 && dst_y == 0 &&
                           width == src_size.Width && height == src_size.Height &&
                           width == dst_size.Width && height == dst_size.Height;
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            const s32 src_layer = copy.src_subresource.base_layer + layer;
            const s32 dst_layer = copy.dst_subresource.base_layer + layer;
            auto* const commands = runtime->scheduler.CommandList();
            if (whole) {
                src.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(D3D12_RESOURCE_STATE_COPY_DEST);
                for (u32 plane = 0; plane < 2; ++plane) {
                    const D3D12_TEXTURE_COPY_LOCATION source{
                        .pResource = src.Handle(),
                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                        .SubresourceIndex =
                            src.Subresource(copy.src_subresource.base_level, src_layer, plane)};
                    const D3D12_TEXTURE_COPY_LOCATION target{
                        .pResource = resource.Get(),
                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                        .SubresourceIndex =
                            Subresource(copy.dst_subresource.base_level, dst_layer, plane)};
                    commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                }
                continue;
            }
            // Part of a subresource: both go through footprints, the region is copied row by
            // row between them, and the destination returns whole.
            ComPtr<ID3D12Resource> src_planes =
                runtime->transfer_buffers.Acquire(src_footprints.size);
            ComPtr<ID3D12Resource> dst_planes =
                runtime->transfer_buffers.Acquire(dst_footprints.size);
            src.CopyPlanes(copy.src_subresource.base_level, src_layer, src_planes.Get(),
                           src_footprints, true);
            CopyPlanes(copy.dst_subresource.base_level, dst_layer, dst_planes.Get(),
                       dst_footprints, true);
            TransitionBuffer(commands, src_planes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (u32 plane = 0; plane < 2; ++plane) {
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& from = src_footprints.planes[plane];
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& to = dst_footprints.planes[plane];
                const u32 bytes = PlaneTexelBytes(plane);
                for (u32 row = 0; row < height; ++row) {
                    commands->CopyBufferRegion(
                        dst_planes.Get(),
                        to.Offset + static_cast<u64>(dst_y + row) * to.Footprint.RowPitch +
                            static_cast<u64>(dst_x) * bytes,
                        src_planes.Get(),
                        from.Offset + static_cast<u64>(src_y + row) * from.Footprint.RowPitch +
                            static_cast<u64>(src_x) * bytes,
                        static_cast<u64>(width) * bytes);
                }
            }
            TransitionBuffer(commands, dst_planes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            CopyPlanes(copy.dst_subresource.base_level, dst_layer, dst_planes.Get(),
                       dst_footprints, false);
            runtime->transfer_buffers.Release(std::move(src_planes));
            runtime->transfer_buffers.Release(std::move(dst_planes));
        }
    }
}

} // namespace D3D12
