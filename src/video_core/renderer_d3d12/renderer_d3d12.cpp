// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "common/logging.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#include "video_core/surface.h"
#include "video_core/textures/decoders.h"

namespace D3D12 {

namespace {

IUnknown* CoreWindowOf(const Core::Frontend::EmuWindow& emu_window) {
    const auto& info = emu_window.GetWindowInfo();
    if (info.type != Core::Frontend::WindowSystemType::CoreWindow ||
        info.render_surface == nullptr) {
        throw std::runtime_error("D3D12: the frontend did not provide a CoreWindow");
    }
    return static_cast<IUnknown*>(info.render_surface);
}

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
        return 0xFFFF00FFu; // magenta: a format phase 1 does not convert
    }
}

} // Anonymous namespace

RendererD3D12::RendererD3D12(Core::Frontend::EmuWindow& emu_window,
                             Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                             std::unique_ptr<Core::Frontend::GraphicsContext> context_)
    : RendererBase(emu_window, std::move(context_)), device_memory{device_memory_}, gpu{gpu_},
      device{}, swapchain{device, CoreWindowOf(emu_window),
                          emu_window.GetFramebufferLayout().width,
                          emu_window.GetFramebufferLayout().height},
      rasterizer{gpu_} {
    ID3D12Device* const dev = device.Get();

    const D3D12_RESOURCE_DESC image_desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = swapchain.Width(),
        .Height = swapchain.Height(),
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = Swapchain::FORMAT,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    dev->GetCopyableFootprints(&image_desc, 0, 1, 0, &footprint, nullptr, nullptr, &upload_size);

    const D3D12_HEAP_PROPERTIES upload_heap{.Type = D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_RESOURCE_DESC buffer_desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = upload_size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    for (Frame& frame : frames) {
        ThrowIfFailed(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&frame.allocator)),
                      "CreateCommandAllocator");
        ThrowIfFailed(dev->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                                                   &buffer_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&frame.upload)),
                      "CreateCommittedResource (upload)");
        void* mapped{};
        ThrowIfFailed(frame.upload->Map(0, nullptr, &mapped), "ID3D12Resource::Map");
        frame.mapped = static_cast<u8*>(mapped);
    }
    ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         frames[0].allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&command_list)),
                  "CreateCommandList");
    ThrowIfFailed(command_list->Close(), "ID3D12GraphicsCommandList::Close");

    // Present one dark-blue frame straight away: on-console, a blue screen before the guest draws
    // anything proves the device and swapchain work independently of the emulation.
    const u32 index = swapchain.CurrentIndex();
    for (u32 y = 0; y < swapchain.Height(); ++y) {
        auto* row = reinterpret_cast<u32*>(frames[index].mapped + y * footprint.Footprint.RowPitch);
        std::fill_n(row, swapchain.Width(), 0xFF402010u);
    }
    Present(index);
}

RendererD3D12::~RendererD3D12() {
    try {
        device.WaitIdle();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "{}", e.what());
    }
}

void RendererD3D12::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return;
    }
    if (!present_failed) {
        try {
            const u32 index = swapchain.CurrentIndex();
            device.Wait(frames[index].fence_value);
            DrawLayer(framebuffers.front(), frames[index].mapped);
            Present(index);
        } catch (const std::exception& e) {
            // Keep the emulation running headless rather than taking the GPU thread down.
            LOG_CRITICAL(Render, "{} - presentation disabled", e.what());
            present_failed = true;
        }
    }
    gpu.RendererFrameEndNotify();
    render_window.OnFrameDisplayed();
}

void RendererD3D12::DrawLayer(const Tegra::FramebufferConfig& framebuffer, u8* dst) {
    const u32 dst_width = swapchain.Width();
    const u32 dst_height = swapchain.Height();
    const u32 row_pitch = footprint.Footprint.RowPitch;
    for (u32 y = 0; y < dst_height; ++y) {
        std::memset(dst + y * row_pitch, 0, dst_width * 4);
    }

    const u32 bpp = BytesPerPixel(framebuffer);
    const u8* const guest = device_memory.GetPointer<u8>(framebuffer.address + framebuffer.offset);
    if (guest == nullptr || bpp == 0 || framebuffer.width == 0 || framebuffer.height == 0) {
        return;
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
    const int src_width = crop.GetWidth();
    const int src_height = crop.GetHeight();
    if (src_width <= 0 || src_height <= 0) {
        return;
    }

    const auto flags = framebuffer.transform_flags;
    const bool flip_h =
        True(flags & Service::android::BufferTransformFlags::FlipH);
    const bool flip_v =
        True(flags & Service::android::BufferTransformFlags::FlipV);

    // Aspect-preserving letterbox, nearest sampling. Phase 1 only: the shader path replaces this.
    const auto& screen = render_window.GetFramebufferLayout().screen;
    const u32 out_left = std::min<u32>(screen.left, dst_width);
    const u32 out_top = std::min<u32>(screen.top, dst_height);
    const u32 out_width = std::min<u32>(screen.GetWidth(), dst_width - out_left);
    const u32 out_height = std::min<u32>(screen.GetHeight(), dst_height - out_top);
    const std::size_t src_pitch = static_cast<std::size_t>(framebuffer.stride) * bpp;
    for (u32 y = 0; y < out_height; ++y) {
        int sy = static_cast<int>(static_cast<u64>(y) * src_height / out_height);
        if (flip_v) {
            sy = src_height - 1 - sy;
        }
        const u8* const src_row = linear.data() + (crop.top + sy) * src_pitch;
        auto* const out = reinterpret_cast<u32*>(dst + (out_top + y) * row_pitch) + out_left;
        for (u32 x = 0; x < out_width; ++x) {
            int sx = static_cast<int>(static_cast<u64>(x) * src_width / out_width);
            if (flip_h) {
                sx = src_width - 1 - sx;
            }
            out[x] = ToRgba8(framebuffer.pixel_format, src_row + (crop.left + sx) * bpp);
        }
    }
}

void RendererD3D12::Present(u32 image_index) {
    Frame& frame = frames[image_index];
    ID3D12Resource* const image = swapchain.Image(image_index);

    ThrowIfFailed(frame.allocator->Reset(), "ID3D12CommandAllocator::Reset");
    ThrowIfFailed(command_list->Reset(frame.allocator.Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset");

    D3D12_RESOURCE_BARRIER barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition =
            {
                .pResource = image,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = D3D12_RESOURCE_STATE_PRESENT,
                .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST,
            },
    };
    command_list->ResourceBarrier(1, &barrier);

    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = image,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    const D3D12_TEXTURE_COPY_LOCATION src{
        .pResource = frame.upload.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = footprint,
    };
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    command_list->ResourceBarrier(1, &barrier);
    ThrowIfFailed(command_list->Close(), "ID3D12GraphicsCommandList::Close");

    ID3D12CommandList* const lists[] = {command_list.Get()};
    device.Queue()->ExecuteCommandLists(1, lists);
    swapchain.Present();
    frame.fence_value = device.Signal();
}

std::vector<u8> RendererD3D12::GetAppletCaptureBuffer() {
    return std::vector<u8>(VideoCore::Capture::TiledSize);
}

} // namespace D3D12
