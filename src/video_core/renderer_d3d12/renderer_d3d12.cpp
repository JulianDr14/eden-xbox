// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/host_shaders/blit_color_float_frag_spv.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#include "video_core/surface.h"
#include "video_core/textures/decoders.h"

namespace D3D12 {

namespace {

/// Shader-visible CBV/SRV/UAV descriptors in the ring. Far below the 1,000,000 tier limit; enough
/// for many frames of draws with room for the ring never to stall in practice.
constexpr u32 DESCRIPTOR_RING_SIZE = 256 * 1024;

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
        return 0xFFFF00FFu; // magenta: a format this path does not convert yet
    }
}

D3D12_RESOURCE_DESC Texture2DDesc(u32 width, u32 height, DXGI_FORMAT format) {
    return D3D12_RESOURCE_DESC{
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
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    return D3D12_RESOURCE_BARRIER{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition =
            {
                .pResource = resource,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = before,
                .StateAfter = after,
            },
    };
}

/// A staging allocation as the source of a buffer -> texture copy.
D3D12_TEXTURE_COPY_LOCATION StagingSource(const StagingBufferRef& ref,
                                          D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) {
    footprint.Offset = ref.offset;
    return D3D12_TEXTURE_COPY_LOCATION{
        .pResource = ref.buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = footprint,
    };
}

} // Anonymous namespace

RendererD3D12::RendererD3D12(Core::Frontend::EmuWindow& emu_window,
                             Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                             std::unique_ptr<Core::Frontend::GraphicsContext> context_)
    : RendererBase(emu_window, std::move(context_)), device_memory{device_memory_}, gpu{gpu_},
      device{}, swapchain{device, CoreWindowOf(emu_window),
                          emu_window.GetFramebufferLayout().width,
                          emu_window.GetFramebufferLayout().height},
      shader_compiler{}, scheduler{device}, staging_pool{device, scheduler},
      buffer_cache_runtime{device, scheduler, staging_pool, device_memory_},
      view_descriptors{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV},
      sampler_descriptors{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256},
      rtv_descriptors{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256},
      dsv_descriptors{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256},
      texture_cache_runtime{device, scheduler, staging_pool, view_descriptors,
                            sampler_descriptors, rtv_descriptors, dsv_descriptors},
      descriptor_ring{device.Get(), scheduler, DESCRIPTOR_RING_SIZE},
      sampler_heap{device.Get(), scheduler},
      rasterizer{gpu_, device_memory_, device, scheduler, shader_compiler, buffer_cache_runtime,
                 texture_cache_runtime, descriptor_ring, sampler_heap} {
    ID3D12Device* const dev = device.Get();

    const D3D12_RESOURCE_DESC image_desc =
        Texture2DDesc(swapchain.Width(), swapchain.Height(), Swapchain::FORMAT);
    dev->GetCopyableFootprints(&image_desc, 0, 1, 0, &footprint, nullptr, nullptr, &upload_size);
    for (u32 i = 0; i < Swapchain::IMAGE_COUNT; ++i) {
        back_buffer_rtvs[i] = rtv_descriptors.Allocate();
        dev->CreateRenderTargetView(swapchain.Image(i), nullptr, back_buffer_rtvs[i]);
    }

    blit_ready = CreateBlitPipeline();

    // Present one dark-blue frame straight away: on-console, a blue screen before the guest draws
    // anything proves the device and swapchain work independently of the emulation.
    const u32 index = swapchain.CurrentIndex();
    // Force this one-time visible copy through the dedicated path. Guest frames below use the
    // stream ring, so the phase-3a.2 gate exercises both allocation paths.
    StagingBufferRef upload = staging_pool.Request(upload_size, MemoryUsage::Upload, true);
    for (u32 y = 0; y < swapchain.Height(); ++y) {
        auto* row = reinterpret_cast<u32*>(upload.mapped_span.data() +
                                           y * footprint.Footprint.RowPitch);
        std::fill_n(row, swapchain.Width(), 0xFF402010u);
    }
    RecordCopy(upload, swapchain.Image(index));
    staging_pool.FreeDeferred(upload);
    Present(index);
    buffer_cache_runtime.RunSelfTest();
    texture_cache_runtime.RunSelfTest();
    LOG_INFO(Render, "D3D12: presenting through the scheduler (tick {})", scheduler.CurrentTick());
}

RendererD3D12::~RendererD3D12() {
    try {
        scheduler.Finish();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "{}", e.what());
    }
}

bool RendererD3D12::CreateBlitPipeline() {
    if (!shader_compiler.IsAvailable()) {
        LOG_WARNING(Render, "D3D12: no shader path, presenting through the CPU");
        return false;
    }
    try {
        ID3D12Device* const dev = device.Get();

        // Eden's blit shaders, exactly as the Vulkan backend uses them, linked as guest pipelines
        // will be. No Y flip: the blit has always run unflipped (see docs/xbox_d3d12_phase4.md).
        const std::array<ShaderCompiler::PipelineStage, 2> stages{{
            {FULL_SCREEN_TRIANGLE_VERT_SPV, DXIL_SPIRV_SHADER_VERTEX},
            {BLIT_COLOR_FLOAT_FRAG_SPV, DXIL_SPIRV_SHADER_FRAGMENT},
        }};
        auto compiled = shader_compiler.CompilePipeline(stages, {});
        const std::vector<u8> vs = std::move(compiled[0].dxil);
        const std::vector<u8> ps = std::move(compiled[1].dxil);

        // Push constants {tex_scale, tex_offset} arrive as a CBV in PUSH_CONSTANT_SPACE; the
        // combined sampler at set 0 binding 0 becomes t0 + s0 in space 0. The texture and the
        // sampler come from descriptor tables (ring and sampler heap), as guest draws will.
        const D3D12_DESCRIPTOR_RANGE srv_range{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = 0,
        };
        const D3D12_DESCRIPTOR_RANGE sampler_range{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = 0,
        };
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {.ShaderRegister = 0, .RegisterSpace = PUSH_CONSTANT_SPACE,
                               .Num32BitValues = 4};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {.ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
                               .Num32BitValues = 12};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {.NumDescriptorRanges = 1, .pDescriptorRanges = &srv_range};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable = {.NumDescriptorRanges = 1,
                                     .pDescriptorRanges = &sampler_range};
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC root_desc{
            .NumParameters = 4,
            .pParameters = params,
            .NumStaticSamplers = 0,
            .pStaticSamplers = nullptr,
            .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
        };
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> error;
        const HRESULT hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                       &serialized, &error);
        if (FAILED(hr)) {
            throw std::runtime_error(
                error ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                    error->GetBufferSize())
                      : std::string("D3D12SerializeRootSignature failed"));
        }
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
                                               serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&blit_root_signature)),
                      "CreateRootSignature");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = blit_root_signature.Get();
        pso.VS = {vs.data(), vs.size()};
        pso.PS = {ps.data(), ps.size()};
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = Swapchain::FORMAT;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&blit_pipeline)),
                      "CreateGraphicsPipelineState");

        const D3D12_SAMPLER_DESC sampler{
            .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .MipLODBias = 0.0f,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = {0.0f, 0.0f, 0.0f, 1.0f},
            .MinLOD = 0.0f,
            .MaxLOD = D3D12_FLOAT32_MAX,
        };
        linear_sampler = sampler_descriptors.Allocate();
        dev->CreateSampler(&sampler, linear_sampler);

        LOG_INFO(Render,
                 "D3D12: blit pipeline built from translated SPIR-V (VS {} bytes, PS {} bytes)",
                 vs.size(), ps.size());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "{} - presenting through the CPU", e.what());
        return false;
    }
}

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

void RendererD3D12::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return;
    }
    if (!present_failed) {
        try {
            const Tegra::FramebufferConfig& framebuffer = framebuffers.front();
            const u32 index = swapchain.CurrentIndex();
            // Frame pacing: at most IMAGE_COUNT frames ahead of the GPU.
            scheduler.Wait(present_ticks[index]);
            ID3D12Resource* const image = swapchain.Image(index);
            if (blit_ready && CompositeAccelerated(framebuffer, index)) {
                // One dump per run, once the guest has had two seconds to settle.
                constexpr u32 DUMP_FRAME = 120;
                const bool dump = ++accelerated_frames == DUMP_FRAME;
                std::optional<StagingBufferRef> readback;
                if (dump) {
                    readback = RecordFrameReadback(image);
                }
                Present(index);
                if (readback) {
                    WriteFrameDump(*readback);
                }
            } else if (const bool has_image = ReadGuestLayer(framebuffer);
                       blit_ready && has_image) {
                PrepareGuestImage(static_cast<u32>(crop_width), static_cast<u32>(crop_height));
                // Descriptor allocation may flush when a heap wraps. Do it before obtaining
                // staging memory, whose lifetime is tied to the then-current scheduler tick.
                constexpr u64 LINEAR_SAMPLER_KEY = 0;
                const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table =
                    sampler_heap.GetTable({&LINEAR_SAMPLER_KEY, 1}, {&linear_sampler, 1});
                const D3D12_GPU_DESCRIPTOR_HANDLE srv_table =
                    descriptor_ring.Upload({&guest_srv, 1});
                const StagingBufferRef upload =
                    staging_pool.Request(guest_upload_size, MemoryUsage::Upload);
                CopyGuestImage(framebuffer, upload.mapped_span.data(),
                               guest_footprint.Footprint.RowPitch);
                RecordUpload(upload);
                RecordBlit(image, index, srv_table, sampler_table, {1.0f, 1.0f, 0.0f, 0.0f});
                Present(index);
            } else {
                const StagingBufferRef upload =
                    staging_pool.Request(upload_size, MemoryUsage::Upload);
                ScaleGuestImage(framebuffer, upload.mapped_span.data(),
                                footprint.Footprint.RowPitch);
                RecordCopy(upload, image);
                Present(index);
            }
        } catch (const std::exception& e) {
            // Keep the emulation running headless rather than taking the GPU thread down.
            LOG_CRITICAL(Render, "{} - presentation disabled", e.what());
            present_failed = true;
        }
    }
    gpu.RendererFrameEndNotify();
    // As RendererVulkan::Composite: advances the caches' garbage collection and the fence
    // manager's frame, which otherwise never run.
    try {
        rasterizer.TickFrame();
    } catch (const std::exception& e) {
        LOG_CRITICAL(Render, "{} - rasterizer frame tick failed", e.what());
    }
    render_window.OnFrameDisplayed();
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

bool RendererD3D12::CompositeAccelerated(const Tegra::FramebufferConfig& framebuffer,
                                         u32 image_index) {
    const DAddr framebuffer_addr = framebuffer.address + framebuffer.offset;
    const auto texture = rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr);
    if (!texture || texture->srv.ptr == 0 || texture->width == 0 || texture->height == 0) {
        if (accelerated_frames != 0 && !logged_fallback) {
            // Alternating GPU and CPU frames flicker: the CPU path shows guest memory, which
            // GPU rendering never writes. Seen when render target changes went unnoticed.
            LOG_WARNING(Render, "D3D12: framebuffer {:#x} is not a GPU image after {} GPU frames; "
                        "presenting it from guest memory", framebuffer_addr, accelerated_frames);
            logged_fallback = true;
        }
        return false;
    }
    // Same crop and flips as the Vulkan presenter (Tegra::NormalizeCrop): the screen's top edge
    // samples crop.top and its bottom edge crop.bottom. The blit's quad runs from the bottom of
    // the screen (y = -1 in D3D12 clip space, coordinate offset) to the top (offset + scale).
    const Common::Rectangle<f32> crop =
        Tegra::NormalizeCrop(framebuffer, texture->width, texture->height);
    const std::array<float, 4> scale_offset{crop.right - crop.left, crop.top - crop.bottom,
                                            crop.left, crop.bottom};
    constexpr u64 LINEAR_SAMPLER_KEY = 0;
    const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table =
        sampler_heap.GetTable({&LINEAR_SAMPLER_KEY, 1}, {&linear_sampler, 1});
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = descriptor_ring.Upload({&texture->srv, 1});
    RecordBlit(swapchain.Image(image_index), image_index, srv_table, sampler_table, scale_offset);
    if (!logged_accelerated) {
        LOG_INFO(Render, "D3D12: presenting the GPU-rendered guest image ({}x{} image, {}x{} "
                 "framebuffer)", texture->width, texture->height, framebuffer.width,
                 framebuffer.height);
        logged_accelerated = true;
    }
    return true;
}

void RendererD3D12::RecordUpload(const StagingBufferRef& upload) {
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    D3D12_RESOURCE_BARRIER barrier = Transition(guest_texture.Get(),
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
    barrier = Transition(guest_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &barrier);
}

void RendererD3D12::RecordBlit(ID3D12Resource* image, u32 image_index,
                               D3D12_GPU_DESCRIPTOR_HANDLE srv_table,
                               D3D12_GPU_DESCRIPTOR_HANDLE sampler_table,
                               const std::array<float, 4>& tex_scale_offset) {
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    D3D12_RESOURCE_BARRIER barrier =
        Transition(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &barrier);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = back_buffer_rtvs[image_index];
    constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // Letterboxed into the layout's screen rectangle, as the Vulkan presenter does.
    const auto& screen = render_window.GetFramebufferLayout().screen;
    const D3D12_VIEWPORT viewport{
        .TopLeftX = static_cast<float>(screen.left),
        .TopLeftY = static_cast<float>(screen.top),
        .Width = static_cast<float>(screen.GetWidth()),
        .Height = static_cast<float>(screen.GetHeight()),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(swapchain.Width()),
                             static_cast<LONG>(swapchain.Height())};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    // Descriptor heaps are command-list state. The allocations were completed before any staging
    // request or command recording, so no operation below can reset this list before the draw.
    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);

    cmd->SetGraphicsRootSignature(blit_root_signature.Get());
    cmd->SetPipelineState(blit_pipeline.Get());
    // tex_scale, tex_offset
    cmd->SetGraphicsRoot32BitConstants(0, 4, tex_scale_offset.data(), 0);
    const u32 runtime_data[12]{};
    cmd->SetGraphicsRoot32BitConstants(1, 12, runtime_data, 0);
    cmd->SetGraphicsRootDescriptorTable(2, srv_table);
    cmd->SetGraphicsRootDescriptorTable(3, sampler_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    barrier = Transition(image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cmd->ResourceBarrier(1, &barrier);
}

void RendererD3D12::RecordCopy(const StagingBufferRef& upload, ID3D12Resource* image) {
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    D3D12_RESOURCE_BARRIER barrier =
        Transition(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->ResourceBarrier(1, &barrier);
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = image,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    const D3D12_TEXTURE_COPY_LOCATION src = StagingSource(upload, footprint);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1, &barrier);
}

void RendererD3D12::Present(u32 image_index) {
    present_ticks[image_index] = scheduler.Flush();
    swapchain.Present();
    staging_pool.TickFrame();
}

StagingBufferRef RendererD3D12::RecordFrameReadback(ID3D12Resource* image) {
    StagingBufferRef readback = staging_pool.Request(upload_size, MemoryUsage::Download, true);
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    D3D12_RESOURCE_BARRIER barrier =
        Transition(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &barrier);
    const D3D12_TEXTURE_COPY_LOCATION src{
        .pResource = image,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    const D3D12_TEXTURE_COPY_LOCATION dst = StagingSource(readback, footprint);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1, &barrier);
    return readback;
}

void RendererD3D12::WriteFrameDump(StagingBufferRef& readback) {
    scheduler.Finish();
    const u32 width = swapchain.Width();
    const u32 height = swapchain.Height();
    const u32 row_bytes = width * 4;
    const u32 image_bytes = row_bytes * height;
    // 32-bit BGRA bottom-up BMP: BITMAPFILEHEADER + BITMAPINFOHEADER, written by hand.
    std::vector<u8> file(54 + static_cast<size_t>(image_bytes));
    const auto put32 = [&file](size_t offset, u32 value) { std::memcpy(&file[offset], &value, 4); };
    const auto put16 = [&file](size_t offset, u16 value) { std::memcpy(&file[offset], &value, 2); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, static_cast<u32>(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, height);
    put16(26, 1);
    put16(28, 32);
    put32(34, image_bytes);
    for (u32 y = 0; y < height; ++y) {
        const u8* const src = readback.mapped_span.data() +
                              static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        u8* const dst = file.data() + 54 + static_cast<size_t>(height - 1 - y) * row_bytes;
        for (u32 x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    staging_pool.FreeDeferred(readback);
    const std::filesystem::path path =
        Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "frame.bmp";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (out) {
        LOG_INFO(Render, "D3D12: presented frame dumped to {} ({}x{})", path.string(), width,
                 height);
    } else {
        LOG_WARNING(Render, "D3D12: could not write the frame dump {}", path.string());
    }
}

std::vector<u8> RendererD3D12::GetAppletCaptureBuffer() {
    return std::vector<u8>(VideoCore::Capture::TiledSize);
}

} // namespace D3D12
