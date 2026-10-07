// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <utility>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"

// Image to image copies and blits.

namespace D3D12 {

using namespace TextureDetail;

void TextureCacheRuntime::CopyImage(Image& dst, Image& src, std::span<const ImageCopy> copies) {
    if (&dst == &src) {
        // Whole-resource state tracking cannot hold COPY_SOURCE and COPY_DEST at once.
        WarnOnce(logged_self_copy, "copies within one image are not supported yet; skipped");
        return;
    }
    if (!src.Handle() || !dst.Handle()) {
        return;
    }
    if (!Image::AreCopyCompatible(src, dst)) {
        // Plain texels on one side, compressed blocks on the other: not copyable.
        WarnOnce(logged_decoded_copy, "copy between a host-decoded and a compressed image ({} -> "
                 "{}) skipped", src.info.format, dst.info.format);
        return;
    }
    if (src.IsDepthStencilPlanar() || dst.IsDepthStencilPlanar()) {
        // Plane by plane, and only between the same host layout (D24S8 or D32S8): anything else
        // reinterprets depth bits and needs a conversion shader.
        if (src.ResourceFormat() != dst.ResourceFormat() || src.info.num_samples > 1 ||
            dst.info.num_samples > 1) {
            WarnOnce(logged_depth_stencil_copy, "depth-stencil copy {} -> {} needs a conversion; "
                     "skipped", src.info.format, dst.info.format);
            return;
        }
        dst.CopyDepthStencilFrom(src, copies);
        return;
    }
    src.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    dst.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    // In the resource's own blocks: a decoded array copies texel by texel.
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(src.TransferFormat().copy_format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(src.TransferFormat().copy_format);
    const u32 dst_block_w = VideoCore::Surface::DefaultBlockWidth(dst.TransferFormat().copy_format);
    if (src.ResourceFormat() != dst.ResourceFormat() && block_w == 1 && dst_block_w == 1) {
        // Guest reinterpretations (RGBA8 texels read as R11G11B10, R32 as RGBA8...) cross DXGI
        // format families, which CopyTextureRegion rejects (the debug layer invalidates the list;
        // without it the result is undefined). The bytes go through a buffer instead.
        CopyThroughBuffer(dst, src, copies);
        return;
    }
    for (const auto& copy : copies) {
        const u32 depth = src.info.type == ImageType::e3D ? copy.extent.depth : 1U;
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            D3D12_TEXTURE_COPY_LOCATION source{.pResource = src.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = src.Subresource(copy.src_subresource.base_level, copy.src_subresource.base_layer + layer)};
            D3D12_TEXTURE_COPY_LOCATION target{.pResource = dst.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = dst.Subresource(copy.dst_subresource.base_level, copy.dst_subresource.base_layer + layer)};
            // Boxes on block-compressed images must cover whole blocks.
            const D3D12_BOX box{static_cast<u32>(copy.src_offset.x), static_cast<u32>(copy.src_offset.y), static_cast<u32>(copy.src_offset.z),
                static_cast<u32>(copy.src_offset.x) + Common::AlignUp(copy.extent.width, block_w),
                static_cast<u32>(copy.src_offset.y) + Common::AlignUp(copy.extent.height, block_h),
                static_cast<u32>(copy.src_offset.z) + depth};
            scheduler.CommandList()->CopyTextureRegion(&target, copy.dst_offset.x, copy.dst_offset.y,
                                                       copy.dst_offset.z, &source, &box);
        }
    }
}
void TextureCacheRuntime::CopyThroughBuffer(Image& dst, Image& src,
                                            std::span<const ImageCopy> copies) {
    const u32 texel_bytes = VideoCore::Surface::BytesPerBlock(src.TransferFormat().copy_format);
    if (texel_bytes != VideoCore::Surface::BytesPerBlock(dst.TransferFormat().copy_format)) {
        WarnOnce(logged_reinterpret_copy, "copy between formats of different texel sizes ({} -> "
                 "{}) skipped", src.info.format, dst.info.format);
        return;
    }
    auto* const commands = scheduler.CommandList();
    for (const auto& copy : copies) {
        const u32 depth = src.info.type == ImageType::e3D ? std::max(1U, copy.extent.depth) : 1U;
        const u32 row_pitch = AlignPitch(copy.extent.width * texel_bytes);
        const u64 size = static_cast<u64>(row_pitch) * copy.extent.height * depth;
        if (size == 0) {
            continue;
        }
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            ComPtr<ID3D12Resource> transfer = transfer_buffers.Acquire(size);
            D3D12_TEXTURE_COPY_LOCATION footprint{.pResource = transfer.Get(),
                                                  .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            footprint.PlacedFootprint.Footprint = {.Format = src.FootprintFormat(),
                                                   .Width = copy.extent.width,
                                                   .Height = copy.extent.height,
                                                   .Depth = depth,
                                                   .RowPitch = row_pitch};
            const D3D12_TEXTURE_COPY_LOCATION source{
                .pResource = src.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = src.Subresource(copy.src_subresource.base_level,
                                                    copy.src_subresource.base_layer + layer)};
            const D3D12_BOX box{static_cast<u32>(copy.src_offset.x),
                                static_cast<u32>(copy.src_offset.y),
                                static_cast<u32>(copy.src_offset.z),
                                static_cast<u32>(copy.src_offset.x) + copy.extent.width,
                                static_cast<u32>(copy.src_offset.y) + copy.extent.height,
                                static_cast<u32>(copy.src_offset.z) + depth};
            commands->CopyTextureRegion(&footprint, 0, 0, 0, &source, &box);
            TransitionBuffer(commands, transfer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            // Same bytes, described in the destination's format.
            footprint.PlacedFootprint.Footprint.Format = dst.FootprintFormat();
            const D3D12_TEXTURE_COPY_LOCATION target{
                .pResource = dst.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = dst.Subresource(copy.dst_subresource.base_level,
                                                    copy.dst_subresource.base_layer + layer)};
            commands->CopyTextureRegion(&target, copy.dst_offset.x, copy.dst_offset.y,
                                        copy.dst_offset.z, &footprint, nullptr);
            transfer_buffers.Release(std::move(transfer));
        }
    }
}

void TextureCacheRuntime::CopyImageMSAA(Image&, Image&, std::span<const ImageCopy>) {
    LOG_WARNING(Render, "D3D12: MSAA texture copies are deferred to phase 5");
}
void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src, std::span<const ImageCopy> copies) {
    if (VideoCore::Surface::BytesPerBlock(dst.info.format) == VideoCore::Surface::BytesPerBlock(src.info.format))
        CopyImage(dst, src, copies);
    else LOG_WARNING(Render, "D3D12: incompatible texture reinterpretation skipped ({} -> {})", src.info.format, dst.info.format);
}
void TextureCacheRuntime::ConvertImage(Framebuffer*, ImageView&, ImageView&) {
    LOG_WARNING(Render, "D3D12: shader texture conversion is deferred to phase 5");
}
void TextureCacheRuntime::BlitImage(Framebuffer*, ImageView& dst, ImageView& src,
                                    const Region2D& dst_region, const Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation) {
    // Blend operations are drawn as copies, as in the Vulkan backend.
    Image* const dst_image = dst.SourceImage();
    Image* const src_image = src.SourceImage();
    if (!dst_image || !src_image || !dst_image->Handle() || !src_image->Handle()) {
        return;
    }
    if (dst_image == src_image) {
        BlitWithinImage(dst, src, dst_region, src_region, filter);
        return;
    }
    if (dst_image->info.num_samples > 1 || src_image->info.num_samples > 1) {
        WarnOnce(logged_msaa_blit, "MSAA blits and resolves need phase 5; skipped");
        return;
    }
    const SurfaceType src_type = VideoCore::Surface::GetFormatType(src.format);
    const SurfaceType dst_type = VideoCore::Surface::GetFormatType(dst.format);
    const bool src_color = src_type == SurfaceType::ColorTexture;
    if (src_color != (dst_type == SurfaceType::ColorTexture)) {
        WarnOnce(logged_mixed_blit, "blit between color and depth ({} -> {}) skipped", src.format,
                 dst.format);
        return;
    }
    const s32 dst_width = dst_region.end.x - dst_region.start.x;
    const s32 dst_height = dst_region.end.y - dst_region.start.y;
    const bool same_extent = dst_width > 0 && dst_height > 0 &&
                             dst_width == src_region.end.x - src_region.start.x &&
                             dst_height == src_region.end.y - src_region.start.y;
    if (src_color && same_extent && src.format == dst.format) {
        if (!Image::AreCopyCompatible(*src_image, *dst_image)) {
            WarnOnce(logged_decoded_copy, "copy between a host-decoded and a compressed image ({} "
                     "-> {}) skipped", src.format, dst.format);
            return;
        }
        // An unscaled copy: exact for every format, integers included.
        src_image->Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
        dst_image->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_TEXTURE_COPY_LOCATION source{
            .pResource = src_image->Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = src_image->Subresource(src.range.base.level, src.range.base.layer)};
        const D3D12_TEXTURE_COPY_LOCATION target{
            .pResource = dst_image->Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = dst_image->Subresource(dst.range.base.level, dst.range.base.layer)};
        const D3D12_BOX box{static_cast<u32>(src_region.start.x),
                            static_cast<u32>(src_region.start.y), 0,
                            static_cast<u32>(src_region.end.x),
                            static_cast<u32>(src_region.end.y), 1};
        scheduler.CommandList()->CopyTextureRegion(&target, static_cast<u32>(dst_region.start.x),
                                                   static_cast<u32>(dst_region.start.y), 0,
                                                   &source, &box);
        return;
    }
    if (!blit_helper || !blit_helper->IsAvailable()) {
        WarnOnce(logged_no_blit_helper, "scaled blits need the shader path; skipped");
        return;
    }
    const VideoCommon::Extent2D src_size{src.size.width, src.size.height};
    if (src_color) {
        if (VideoCore::Surface::IsPixelFormatInteger(src.format) ||
            VideoCore::Surface::IsPixelFormatInteger(dst.format)) {
            // The blit shader samples and writes floats.
            WarnOnce(logged_integer_blit, "scaled blits of integer formats ({} -> {}) skipped",
                     src.format, dst.format);
            return;
        }
        if (!dst.RenderTarget().ptr) {
            WarnOnce(logged_blit_target, "blit target format {} cannot be rendered to; skipped",
                     dst.format);
            return;
        }
        src_image->Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        src.PrepareRead(Shader::TextureType::Color2D);
        dst.PrepareRender();
        const bool linear = filter == Tegra::Engines::Fermi2D::Filter::Bilinear;
        blit_helper->BlitColor({dst.RenderTarget(), Format(dst.format).view, 1},
                               src.Handle(Shader::TextureType::Color2D),
                               linear ? blit_helper->LinearSampler()
                                      : blit_helper->NearestSampler(),
                               dst_region, src_region, src_size);
        return;
    }
    if (!dst.DepthStencil().ptr) {
        WarnOnce(logged_blit_target, "blit target format {} cannot be rendered to; skipped",
                 dst.format);
        return;
    }
    if (dst_type == SurfaceType::DepthStencil) {
        // Writing stencil from a shader needs SV_StencilRef, which the Series lacks.
        WarnOnce(logged_stencil_blit, "depth-stencil blits copy depth only ({})", dst.format);
    }
    src_image->Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    dst_image->Transition(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    blit_helper->BlitDepth({dst.DepthStencil(), Format(dst.format).dsv, 1},
                           src.Handle(Shader::TextureType::Color2D), dst_region, src_region,
                           src_size);
}
void TextureCacheRuntime::BlitWithinImage(ImageView& dst, ImageView& src,
                                          const Region2D& dst_region, const Region2D& src_region,
                                          Tegra::Engines::Fermi2D::Filter filter) {
    // Whole-resource state tracking cannot hold a shader resource and a target at once, and a
    // copy cannot read and write one subresource; a scratch copy of the source region can.
    Image& image = *src.SourceImage();
    const PixelFormat image_format = image.info.format;
    const bool color = VideoCore::Surface::GetFormatType(src.format) == SurfaceType::ColorTexture &&
                       VideoCore::Surface::GetFormatType(dst.format) == SurfaceType::ColorTexture;
    const FormatInfo src_format = Format(src.format);
    if (!color || image.info.num_samples > 1 || image.info.type == ImageType::e3D ||
        image.IsBcDecoded() || image.IsGpuDecoded() ||
        VideoCore::Surface::DefaultBlockWidth(image_format) != 1 ||
        VideoCore::Surface::DefaultBlockHeight(image_format) != 1 ||
        TypelessFamily(src_format.srv) != TypelessFamily(image.ResourceFormat())) {
        WarnOnce(logged_self_blit, "blit within one {} image ({} -> {}) skipped", image_format,
                 src.format, dst.format);
        return;
    }
    const s32 dst_width = dst_region.end.x - dst_region.start.x;
    const s32 dst_height = dst_region.end.y - dst_region.start.y;
    const s32 src_width = src_region.end.x - src_region.start.x;
    const s32 src_height = src_region.end.y - src_region.start.y;
    const bool copy = dst_width > 0 && dst_height > 0 && dst_width == src_width &&
                      dst_height == src_height && src.format == dst.format;
    if (!copy) {
        if (!blit_helper || !blit_helper->IsAvailable()) {
            WarnOnce(logged_no_blit_helper, "scaled blits need the shader path; skipped");
            return;
        }
        if (VideoCore::Surface::IsPixelFormatInteger(src.format) ||
            VideoCore::Surface::IsPixelFormatInteger(dst.format)) {
            WarnOnce(logged_integer_blit, "scaled blits of integer formats ({} -> {}) skipped",
                     src.format, dst.format);
            return;
        }
        if (!dst.RenderTarget().ptr) {
            WarnOnce(logged_blit_target, "blit target format {} cannot be rendered to; skipped",
                     dst.format);
            return;
        }
    }
    // The source rectangle, flips undone, clamped to its level.
    const s32 level = src.range.base.level;
    const s32 level_width = static_cast<s32>(std::max(1U, image.info.size.width >> level));
    const s32 level_height = static_cast<s32>(std::max(1U, image.info.size.height >> level));
    const s32 left = std::clamp(std::min(src_region.start.x, src_region.end.x), 0, level_width);
    const s32 top = std::clamp(std::min(src_region.start.y, src_region.end.y), 0, level_height);
    const s32 right = std::clamp(std::max(src_region.start.x, src_region.end.x), 0, level_width);
    const s32 bottom = std::clamp(std::max(src_region.start.y, src_region.end.y), 0, level_height);
    if (right <= left || bottom <= top) {
        return;
    }
    const u32 width = static_cast<u32>(right - left);
    const u32 height = static_cast<u32>(bottom - top);
    EnsureSelfBlitScratch(image.ResourceFormat(), width, height);
    ID3D12GraphicsCommandList* const commands = scheduler.CommandList();
    image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionSelfBlitScratch(D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_TEXTURE_COPY_LOCATION image_source{
        .pResource = image.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = image.Subresource(level, src.range.base.layer)};
    const D3D12_TEXTURE_COPY_LOCATION scratch{
        .pResource = self_blit_scratch.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0};
    const D3D12_BOX box{static_cast<u32>(left), static_cast<u32>(top), 0,
                        static_cast<u32>(right), static_cast<u32>(bottom), 1};
    commands->CopyTextureRegion(&scratch, 0, 0, 0, &image_source, &box);
    if (copy) {
        // An unscaled copy: exact for every format, integers included.
        TransitionSelfBlitScratch(D3D12_RESOURCE_STATE_COPY_SOURCE);
        image.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_TEXTURE_COPY_LOCATION target{
            .pResource = image.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = image.Subresource(dst.range.base.level, dst.range.base.layer)};
        const D3D12_BOX scratch_box{0, 0, 0, width, height, 1};
        commands->CopyTextureRegion(&target, static_cast<u32>(dst_region.start.x),
                                    static_cast<u32>(dst_region.start.y), 0, &scratch,
                                    &scratch_box);
        return;
    }
    TransitionSelfBlitScratch(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const D3D12_CPU_DESCRIPTOR_HANDLE srv = view_descriptors.Allocate();
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = src_format.srv,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0,
                      .ResourceMinLODClamp = 0.0f},
    };
    device.Get()->CreateShaderResourceView(self_blit_scratch.Get(), &srv_desc, srv);
    // Same orientation as the guest's region, moved to where the scratch holds it.
    const Region2D scratch_region{
        .start = {src_region.start.x - left, src_region.start.y - top},
        .end = {src_region.end.x - left, src_region.end.y - top},
    };
    dst.PrepareRender();
    const bool linear = filter == Tegra::Engines::Fermi2D::Filter::Bilinear;
    blit_helper->BlitColor({dst.RenderTarget(), Format(dst.format).view, 1}, srv,
                           linear ? blit_helper->LinearSampler() : blit_helper->NearestSampler(),
                           dst_region, scratch_region, {self_blit_width, self_blit_height});
    // The helper copies the descriptor into the shader-visible ring while recording.
    view_descriptors.Free(srv);
}

void TextureCacheRuntime::EnsureSelfBlitScratch(DXGI_FORMAT format, u32 width, u32 height) {
    if (self_blit_scratch && self_blit_format == format && self_blit_width >= width &&
        self_blit_height >= height) {
        return;
    }
    if (self_blit_scratch) {
        if (self_blit_format == format) {
            width = std::max(width, self_blit_width);
            height = std::max(height, self_blit_height);
        }
        scheduler.DeferRelease(std::move(self_blit_scratch));
    }
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(device.Get()->CreateCommittedResource(
                      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                      IID_PPV_ARGS(&self_blit_scratch)),
                  "Create self-blit scratch texture");
    self_blit_state = D3D12_RESOURCE_STATE_COMMON;
    self_blit_format = format;
    self_blit_width = width;
    self_blit_height = height;
    LOG_INFO(Render, "D3D12: blits within one image go through a {}x{} scratch (DXGI {})", width,
             height, static_cast<u32>(format));
}

void TextureCacheRuntime::TransitionSelfBlitScratch(D3D12_RESOURCE_STATES next) {
    if (self_blit_state == next) {
        return;
    }
    const D3D12_RESOURCE_BARRIER barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {.pResource = self_blit_scratch.Get(),
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = self_blit_state,
                       .StateAfter = next}};
    scheduler.CommandList()->ResourceBarrier(1, &barrier);
    self_blit_state = next;
}

} // namespace D3D12
