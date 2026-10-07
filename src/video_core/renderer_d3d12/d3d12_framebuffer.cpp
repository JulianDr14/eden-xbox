// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>

#include "common/settings.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/render_targets.h"

namespace D3D12 {

using namespace TextureDetail;

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, std::span<ImageView*, NUM_RT> color_buffers,
                         ImageView* depth_buffer, const VideoCommon::RenderTargets& key)
    : extent{key.size}, is_rescaled{key.is_rescaled} {
    // Slot i is guest render target i, as in the Vulkan render pass and the pipeline's RTVFormats
    // (fragment output i writes render target i; rt_control's mapping is not applied).
    colors.fill(runtime.NullRenderTarget());
    for (size_t index = 0; index < color_buffers.size(); ++index) {
        ImageView* const view = color_buffers[index];
        if (!view) continue;
        if (!view->RenderTarget().ptr) {
            missing_colors |= 1U << index;
            WarnOnce(logged_missing_rtv, "render target {} (format {}) has no RTV; draws to it "
                     "are lost", index, view->format);
            continue;
        }
        colors[index] = view->RenderTarget();
        color_images[index] = view->image_id;
        if (view->RenderTargetOnCopy()) {
            copy_colors |= 1U << index;
        }
        color_formats[index] = runtime.Format(view->format).view;
        color_bases[index] = view->range.base;
        color_layers[index] = static_cast<u32>(view->range.extent.layers);
        images = view->slot_images ? view->slot_images : images;
        num_colors = static_cast<u32>(index) + 1;
        if (const Image* const image = view->SourceImage()) {
            samples = std::max(1U, image->info.num_samples);
        }
    }
    // D3D12 only binds a depth view at least as large as every render target; drivers accept a
    // smaller one (Vulkan draws in the intersection), but the debug layer invalidates the command
    // list. With the debug layer (PC), such a pass is drawn without depth so the run goes on.
    bool depth_too_small = false;
    if (depth_buffer && Settings::values.renderer_debug.GetValue()) {
        for (size_t index = 0; index < num_colors; ++index) {
            const ImageView* const view = color_buffers[index];
            if (view && (view->size.width > depth_buffer->size.width ||
                         view->size.height > depth_buffer->size.height)) {
                depth_too_small = true;
            }
        }
        if (depth_too_small) {
            WarnOnce(logged_depth_size, "depth buffer smaller than a render target ({}x{}); "
                     "drawn without depth under the debug layer", depth_buffer->size.width,
                     depth_buffer->size.height);
        }
    }
    if (depth_buffer && depth_buffer->DepthStencil().ptr && !depth_too_small) {
        depth = depth_buffer->DepthStencil();
        depth_read_only = depth_buffer->DepthStencilReadOnly();
        depth_image = depth_buffer->image_id;
        depth_base = depth_buffer->range.base;
        depth_layers = static_cast<u32>(depth_buffer->range.extent.layers);
        depth_format = runtime.Format(depth_buffer->format).dsv;
        images = depth_buffer->slot_images ? depth_buffer->slot_images : images;
        has_stencil = VideoCore::Surface::GetFormatType(depth_buffer->format) ==
                      SurfaceType::DepthStencil;
        if (const Image* const image = depth_buffer->SourceImage()) {
            samples = std::max(1U, image->info.num_samples);
        }
    }
}

void Framebuffer::PrepareAttachments(bool depth_sampled) const {
    if (!images) return;
    for (size_t index = 0; index < num_colors; ++index) {
        if (color_images[index] == ImageId{}) {
            continue;
        }
        Image& image = (*images)[color_images[index]];
        if ((copy_colors >> index) & 1) {
            image.RenderToReinterpreted();
        } else {
            image.Transition(D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }
    if (depth_image != ImageId{}) {
        (*images)[depth_image].Transition(depth_sampled ? DEPTH_SAMPLED_STATE
                                                        : D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
}

const Image* Framebuffer::ColorImage(size_t index) const noexcept {
    if (!images || index >= NUM_RT || color_images[index] == ImageId{}) return nullptr;
    return &(*images)[color_images[index]];
}

const Image* Framebuffer::DepthImage() const noexcept {
    if (!images || depth_image == ImageId{}) return nullptr;
    return &(*images)[depth_image];
}

D3D12_CPU_DESCRIPTOR_HANDLE TextureCacheRuntime::LayerTarget(const Framebuffer& framebuffer,
                                                             size_t attachment, u32 first,
                                                             u32 count) {
    const bool depth = attachment == NUM_RT;
    const Image* const image = depth ? framebuffer.DepthImage() : framebuffer.ColorImage(attachment);
    if (!image || !image->Handle() || count == 0 || (!depth && framebuffer.ColorOnCopy(attachment))) {
        return {};
    }
    const D3D12_RESOURCE_DESC desc = image->Handle()->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return {};
    }
    const VideoCommon::SubresourceBase base =
        depth ? framebuffer.DepthBase() : framebuffer.ColorBase(attachment);
    const u32 layers = desc.DepthOrArraySize;
    const u32 first_layer = static_cast<u32>(base.layer) + first;
    if (first_layer >= layers) {
        return {};
    }
    count = std::min(count, layers - first_layer);
    const u32 level = static_cast<u32>(base.level);
    const bool msaa = desc.SampleDesc.Count > 1;
    if (depth) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_descriptors.Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{.Format = framebuffer.DepthFormat(),
                                               .Flags = D3D12_DSV_FLAG_NONE};
        if (msaa) {
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            dsv_desc.Texture2DMSArray = {first_layer, count};
        } else {
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsv_desc.Texture2DArray = {level, first_layer, count};
        }
        device.Get()->CreateDepthStencilView(image->Handle(), &dsv_desc, dsv);
        return dsv;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_descriptors.Allocate();
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{.Format = framebuffer.ColorFormat(attachment)};
    if (msaa) {
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        rtv_desc.Texture2DMSArray = {first_layer, count};
    } else {
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv_desc.Texture2DArray = {level, first_layer, count, 0};
    }
    device.Get()->CreateRenderTargetView(image->Handle(), &rtv_desc, rtv);
    return rtv;
}

void TextureCacheRuntime::FreeLayerTarget(D3D12_CPU_DESCRIPTOR_HANDLE handle, bool depth) {
    if (handle.ptr) {
        (depth ? dsv_descriptors : rtv_descriptors).Free(handle);
    }
}

} // namespace D3D12
