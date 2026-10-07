// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

#include "common/logging.h"
#include "core/frontend/emu_window.h"
#include "video_core/framebuffer_config.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#include "video_core/renderer_d3d12/renderer_d3d12_internal.h"
#include "video_core/textures/decoders.h"

// Fallback presentation: the guest framebuffer read from guest memory, converted and scaled on
// the CPU, then uploaded.

namespace D3D12 {

using namespace PresentDetail;

namespace {

u32 BytesPerPixel(const Tegra::FramebufferConfig& framebuffer) {
    using namespace VideoCore::Surface;
    return BytesPerBlock(PixelFormatFromGPUPixelFormat(framebuffer.pixel_format));
}

/// One guest pixel as little-endian RGBA8 (R in the low byte), matching DXGI R8G8B8A8_UNORM.
u32 ToRgba8(Service::android::PixelFormat format, const u8* src) {
    using Service::android::PixelFormat;
    switch (format) {
    case PixelFormat::Rgba8888: {
        u32 v;
        std::memcpy(&v, src, 4);
        return v;
    }
    case PixelFormat::Rgbx8888: {
        u32 v;
        std::memcpy(&v, src, 4);
        return v | 0xFF000000u;
    }
    case PixelFormat::Bgra8888:
        return u32{src[2]} | (u32{src[1]} << 8) | (u32{src[0]} << 16) | (u32{src[3]} << 24);
    case PixelFormat::Rgb565: {
        const u32 v = u32{src[0]} | (u32{src[1]} << 8);
        const u32 r = (v >> 11) & 0x1F;
        const u32 g = (v >> 5) & 0x3F;
        const u32 b = v & 0x1F;
        return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) |
               (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
    }
    default:
        return 0xFFFF00FFu; // magenta: a format this path does not convert yet
    }
}

} // Anonymous namespace

void RendererD3D12::PrepareGuestImage(u32 width, u32 height) {
    if (guest_texture && width == guest_width && height == guest_height) {
        return;
    }
    ID3D12Device* const dev = device.Get();

    // The old texture may still be read by frames in flight: the scheduler keeps it alive.
    scheduler.DeferRelease(std::move(guest_texture));
    const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc = Texture2DDesc(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    ThrowIfFailed(dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               nullptr, IID_PPV_ARGS(&guest_texture)),
                  "CreateCommittedResource (guest texture)");
    if (guest_srv.ptr == 0) {
        guest_srv = view_descriptors.Allocate();
    }
    // Rewriting the offline SRV is safe: frames in flight read their copy in the ring.
    dev->CreateShaderResourceView(guest_texture.Get(), nullptr, guest_srv);
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &guest_footprint, nullptr, nullptr,
                               &guest_upload_size);
    guest_width = width;
    guest_height = height;
    LOG_INFO(Render, "D3D12: guest image {}x{}", width, height);
}

bool RendererD3D12::ReadGuestLayer(const Tegra::FramebufferConfig& framebuffer) {
    const u32 bpp = BytesPerPixel(framebuffer);
    const u8* const guest = device_memory.GetPointer<u8>(framebuffer.address + framebuffer.offset);
    if (guest == nullptr || bpp == 0 || framebuffer.width == 0 || framebuffer.height == 0) {
        return false;
    }

    // Guest display buffers are block linear; same parameters as the Vulkan layer upload.
    constexpr u32 block_height_log2 = 4;
    const std::size_t linear_size =
        static_cast<std::size_t>(framebuffer.stride) * framebuffer.height * bpp;
    const std::size_t tiled_size = Tegra::Texture::CalculateSize(
        true, bpp, framebuffer.stride, framebuffer.height, 1, block_height_log2, 0);
    linear.resize(linear_size);
    Tegra::Texture::UnswizzleTexture(linear, std::span(guest, tiled_size), bpp, framebuffer.width,
                                     framebuffer.height, 1, block_height_log2, 0);

    Common::Rectangle<int> crop = framebuffer.crop_rect;
    if (crop.GetWidth() <= 0 || crop.GetHeight() <= 0) {
        crop = {0, 0, static_cast<int>(framebuffer.width), static_cast<int>(framebuffer.height)};
    }
    crop.right = std::min(crop.right, static_cast<int>(framebuffer.width));
    crop.bottom = std::min(crop.bottom, static_cast<int>(framebuffer.height));
    crop_left = crop.left;
    crop_top = crop.top;
    crop_width = crop.GetWidth();
    crop_height = crop.GetHeight();
    return crop_width > 0 && crop_height > 0;
}

void RendererD3D12::CopyGuestImage(const Tegra::FramebufferConfig& framebuffer, u8* dst,
                                   u32 row_pitch) {
    const u32 bpp = BytesPerPixel(framebuffer);
    const std::size_t src_pitch = static_cast<std::size_t>(framebuffer.stride) * bpp;
    const auto flags = framebuffer.transform_flags;
    const bool flip_h = True(flags & Service::android::BufferTransformFlags::FlipH);
    const bool flip_v = True(flags & Service::android::BufferTransformFlags::FlipV);
    for (int y = 0; y < crop_height; ++y) {
        const int sy = flip_v ? crop_height - 1 - y : y;
        const u8* const src_row = linear.data() + (crop_top + sy) * src_pitch;
        auto* const out = reinterpret_cast<u32*>(dst + static_cast<std::size_t>(y) * row_pitch);
        for (int x = 0; x < crop_width; ++x) {
            const int sx = flip_h ? crop_width - 1 - x : x;
            out[x] = ToRgba8(framebuffer.pixel_format, src_row + (crop_left + sx) * bpp);
        }
    }
}

void RendererD3D12::ScaleGuestImage(const Tegra::FramebufferConfig& framebuffer, u8* dst,
                                    u32 row_pitch) {
    const u32 dst_width = swapchain.Width();
    const u32 dst_height = swapchain.Height();
    for (u32 y = 0; y < dst_height; ++y) {
        std::memset(dst + y * row_pitch, 0, dst_width * 4);
    }
    if (crop_width <= 0 || crop_height <= 0 || linear.empty()) {
        return;
    }
    const u32 bpp = BytesPerPixel(framebuffer);
    const auto flags = framebuffer.transform_flags;
    const bool flip_h = True(flags & Service::android::BufferTransformFlags::FlipH);
    const bool flip_v = True(flags & Service::android::BufferTransformFlags::FlipV);

    // Aspect-preserving letterbox, nearest sampling.
    const auto& screen = render_window.GetFramebufferLayout().screen;
    const u32 out_left = std::min<u32>(screen.left, dst_width);
    const u32 out_top = std::min<u32>(screen.top, dst_height);
    const u32 out_width = std::min<u32>(screen.GetWidth(), dst_width - out_left);
    const u32 out_height = std::min<u32>(screen.GetHeight(), dst_height - out_top);
    const std::size_t src_pitch = static_cast<std::size_t>(framebuffer.stride) * bpp;
    for (u32 y = 0; y < out_height; ++y) {
        int sy = static_cast<int>(static_cast<u64>(y) * crop_height / out_height);
        if (flip_v) {
            sy = crop_height - 1 - sy;
        }
        const u8* const src_row = linear.data() + (crop_top + sy) * src_pitch;
        auto* const out = reinterpret_cast<u32*>(dst + (out_top + y) * row_pitch) + out_left;
        for (u32 x = 0; x < out_width; ++x) {
            int sx = static_cast<int>(static_cast<u64>(x) * crop_width / out_width);
            if (flip_h) {
                sx = crop_width - 1 - sx;
            }
            out[x] = ToRgba8(framebuffer.pixel_format, src_row + (crop_left + sx) * bpp);
        }
    }
}

void RendererD3D12::RecordUpload(const StagingBufferRef& upload) {
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(guest_texture.Get(),
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->ResourceBarrier(1, &barrier);
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = guest_texture.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    const D3D12_TEXTURE_COPY_LOCATION src = StagingSource(upload, guest_footprint);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier = TransitionBarrier(guest_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &barrier);
}

void RendererD3D12::RecordCopy(const StagingBufferRef& upload, ID3D12Resource* image) {
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    D3D12_RESOURCE_BARRIER barrier =
        TransitionBarrier(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->ResourceBarrier(1, &barrier);
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = image,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    const D3D12_TEXTURE_COPY_LOCATION src = StagingSource(upload, footprint);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier = TransitionBarrier(image, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &barrier);
    DrawPerformanceOverlay(cmd, back_buffer_rtvs[swapchain.CurrentIndex()]);
    barrier = TransitionBarrier(image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cmd->ResourceBarrier(1, &barrier);
}

} // namespace D3D12
