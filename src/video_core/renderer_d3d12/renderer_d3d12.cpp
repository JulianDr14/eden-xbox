// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>


#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/cpu_profile.h"
#include "dynarmic/interface/jit_profile.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/framebuffer_config.h"
#include "video_core/frame_trace.h"
#include "video_core/gpu.h"
#include "video_core/host_shaders/blit_color_float_frag_spv.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#include "video_core/shader_notify.h"
#include "video_core/surface.h"
#include "video_core/textures/decoders.h"

namespace D3D12 {

namespace {

/// Shader-visible CBV/SRV/UAV descriptors in the ring. Far below the 1,000,000 tier limit; enough
/// for many frames of draws with room for the ring never to stall in practice.
constexpr u32 DESCRIPTOR_RING_SIZE = 256 * 1024;

std::atomic<u32> traced_frame_override{};
std::atomic<bool> frame_diagnostics{true};

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

/// Shared 3x5 overlay font, drawn as batched ClearRenderTargetView rects.
/// Digits 0-9 then '/'; rows top to bottom, bit 2 is the left column.
constexpr std::array<std::array<u8, 5>, 11> GLYPHS{{
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
    {1, 1, 2, 4, 4},
}};

constexpr std::array<std::array<u8, 5>, 26> LETTER_GLYPHS{{
    {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7},
    {7,4,6,4,4}, {3,4,5,5,3}, {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2},
    {5,5,6,5,5}, {4,4,4,4,7}, {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2},
    {6,5,6,4,4}, {2,5,5,3,1}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2},
    {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2},
    {7,1,2,4,7},
}};

std::array<u8, 5> Glyph(char c) {
    if (c >= '0' && c <= '9') {
        return GLYPHS[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return LETTER_GLYPHS[c - 'A'];
    }
    switch (c) {
    case '/': return GLYPHS[10];
    case '.': return {0,0,0,0,2};
    case '%': return {5,1,2,4,5};
    case '-': return {0,0,7,0,0};
    default: return {};
    }
}

/// Width of `chars` glyphs of `cell` pixels, with a one-cell gap between them.
LONG TextWidth(size_t chars, LONG cell) {
    return chars == 0 ? 0 : static_cast<LONG>(chars) * 4 * cell - cell;
}

/// Shared bitmap text for shader progress and performance panels. Spaces advance normally.
void AppendText(std::vector<D3D12_RECT>& rects, std::string_view text, LONG x, LONG y, LONG cell) {
    for (const char c : text) {
        const auto glyph = Glyph(c);
        for (LONG row = 0; row < 5; ++row) {
            LONG column = 0;
            while (column < 3) {
                if (((glyph[row] >> (2 - column)) & 1) == 0) {
                    ++column;
                    continue;
                }
                const LONG start = column++;
                while (column < 3 && ((glyph[row] >> (2 - column)) & 1)) {
                    ++column;
                }
                rects.push_back({x + start * cell, y + row * cell, x + column * cell,
                                 y + (row + 1) * cell});
            }
        }
        x += 4 * cell;
    }
}

void ClearRects(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                const std::array<float, 4>& color, std::span<const D3D12_RECT> rects) {
    if (!rects.empty()) {
        cmd->ClearRenderTargetView(rtv, color.data(), static_cast<UINT>(rects.size()),
                                   rects.data());
    }
}

// Overlay colors: a near-black panel, dim and amber activity dots, light gray text.
constexpr std::array<float, 4> OVERLAY_PANEL{0.04f, 0.04f, 0.05f, 1.0f};
constexpr std::array<float, 4> OVERLAY_DIM{0.25f, 0.25f, 0.28f, 1.0f};
constexpr std::array<float, 4> OVERLAY_ACCENT{1.0f, 0.72f, 0.2f, 1.0f};
constexpr std::array<float, 4> OVERLAY_TEXT{0.82f, 0.82f, 0.84f, 1.0f};

} // Anonymous namespace

void ShowLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total) {
    // No RTTI in this build: the configured backend says which renderer this is.
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowLoadProgress(done, total);
    }
}

void SetTracedFrame(u32 frame) {
    traced_frame_override.store(frame, std::memory_order_relaxed);
}

void SetFrameDiagnostics(bool enabled) {
    frame_diagnostics.store(enabled, std::memory_order_relaxed);
}

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
      blit_helper{device, scheduler, shader_compiler, descriptor_ring, sampler_heap,
                  sampler_descriptors},
      rasterizer{gpu_, device_memory_, device, scheduler, shader_compiler, buffer_cache_runtime,
                 texture_cache_runtime, descriptor_ring, sampler_heap, blit_helper,
                 staging_pool} {
    ID3D12Device* const dev = device.Get();
    texture_cache_runtime.SetBlitHelper(&blit_helper);

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
    // Historical boot gates are opt-in during development, outside FPS trials.
    constexpr bool run_boot_self_tests = false;
    if constexpr (run_boot_self_tests) {
        buffer_cache_runtime.RunSelfTest();
        texture_cache_runtime.RunSelfTest();
    }
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
            const auto wait_start = std::chrono::steady_clock::now();
            scheduler.Wait(present_ticks[index]);
            const auto wait_end = std::chrono::steady_clock::now();
            ID3D12Resource* const image = swapchain.Image(index);
            if (blit_ready && CompositeAccelerated(framebuffer, index)) {
                // frame.bmp once the guest has had two seconds to settle, then frame_<n>.bmp
                // every ten seconds or so, to follow a game's progress in a headless run.
                constexpr u32 DUMP_FRAME = 120;
                constexpr u32 DUMP_INTERVAL = 600;
                constexpr u32 MAX_DUMPS = 12;
                ++accelerated_frames;
                const bool diagnostics = frame_diagnostics.load(std::memory_order_relaxed);
                const bool dump = diagnostics && accelerated_frames >= DUMP_FRAME &&
                                  (accelerated_frames - DUMP_FRAME) % DUMP_INTERVAL == 0 &&
                                  (accelerated_frames - DUMP_FRAME) / DUMP_INTERVAL < MAX_DUMPS;
                std::optional<StagingBufferRef> readback;
                if (dump) {
                    readback = RecordFrameReadback(image);
                }
                Present(index);
                if (readback) {
                    WriteFrameDump(*readback, (accelerated_frames - DUMP_FRAME) / DUMP_INTERVAL);
                }
                // Every draw of the frame that ends in frame_1.bmp goes to the log, to compare
                // a console run with a PC run draw by draw.
                const u32 override_frame = traced_frame_override.load(std::memory_order_relaxed);
                const u32 traced_frame =
                    override_frame != 0 ? override_frame : DUMP_FRAME + DUMP_INTERVAL;
                // Played by hand (boot.cfg "play=1"): no frame dumps, no draw trace.
                if (diagnostics && accelerated_frames == traced_frame - 1) {
                    LOG_INFO(Render, "D3D12: tracing the draws of frame {}", traced_frame);
                    // An explicit trace_frame also writes its render targets (trace\*.bmp).
                    rasterizer.SetDrawTrace(true, override_frame != 0);
                } else if (diagnostics && accelerated_frames == traced_frame) {
                    rasterizer.SetDrawTrace(false);
                    LOG_INFO(Render, "D3D12: draw trace of frame {} complete", traced_frame);
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
            const auto present_end = std::chrono::steady_clock::now();
            using Ms = std::chrono::duration<double, std::milli>;
            RecordPacing(Ms(wait_end - wait_start).count(), Ms(present_end - wait_end).count());
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

void RendererD3D12::RecordPacing(double wait_ms, double present_ms) {
    device.LogDebugMessages();
    constexpr u32 PACING_WINDOW = PacingStats::WINDOW;
    constexpr double HITCH_MS = 1000.0 / 60.0 * 1.5;
    const auto now = std::chrono::steady_clock::now();
    if (pacing.last_composite != std::chrono::steady_clock::time_point{}) {
        const double interval =
            std::chrono::duration<double, std::milli>(now - pacing.last_composite).count();
        pacing.intervals_ms[pacing.frames++] = interval;
        ++performance_overlay.frames;
        performance_overlay.last_frame_ms = interval;
        performance_overlay.max_frame_ms = std::max(performance_overlay.max_frame_ms, interval);
        pacing.total_ms += interval;
        pacing.max_interval_ms = std::max(pacing.max_interval_ms, interval);
        pacing.hitches += interval > HITCH_MS ? 1 : 0;
        pacing.max_wait_ms = std::max(pacing.max_wait_ms, wait_ms);
        pacing.max_present_ms = std::max(pacing.max_present_ms, present_ms);
        ReportPerf(interval);
    } else {
        perf_frame = VideoCore::Perf::Read();
        perf_window = perf_frame;
    }
    pacing.last_composite = now;
    if (pacing.frames == PACING_WINDOW) {
        // Nearest-rank percentiles over presented intervals. Sort once per window, with no
        // allocation or extra clocks per frame. p99 exposes hitches hidden by the mean.
        auto sorted = pacing.intervals_ms;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](u32 percent) {
            return sorted[(PACING_WINDOW * percent + 99) / 100 - 1];
        };
        LOG_INFO(Render,
                 "D3D12 pacing: {} frames, avg {:.2f} ms, max {:.2f} ms, {} hitches; max wait "
                 "{:.2f} ms, max record+present {:.2f} ms",
                 pacing.frames, pacing.total_ms / pacing.frames, pacing.max_interval_ms,
                 pacing.hitches, pacing.max_wait_ms, pacing.max_present_ms);
        LOG_INFO(Render, "D3D12 frame times: {:.2f} FPS, p50 {:.2f} ms, p95 {:.2f} ms, "
                         "p99 {:.2f} ms ({} presented intervals)",
                 1000.0 * pacing.frames / pacing.total_ms, percentile(50), percentile(95),
                 percentile(99), pacing.frames);
        ReportPerfWindow(pacing.frames, pacing.total_ms);
        pacing = PacingStats{.last_composite = now};
    }
}

namespace {

using VideoCore::Perf::Counter;

/// The change of every counter between two snapshots.
VideoCore::Perf::Snapshot Delta(const VideoCore::Perf::Snapshot& now,
                                const VideoCore::Perf::Snapshot& before) {
    VideoCore::Perf::Snapshot delta{};
    for (size_t i = 0; i < delta.size(); ++i) {
        delta[i] = now[i] - before[i];
    }
    return delta;
}

double Ms(const VideoCore::Perf::Snapshot& delta, Counter counter) {
    return static_cast<double>(VideoCore::Perf::Get(delta, counter)) / 1000.0;
}

/// Counters of one frame or window, one line. The GPU thread's time splits into idle (waiting
/// for the guest), fence waits (waiting for the GPU), pipeline stalls, guest flushes, resource
/// creation, and the rest: recording draws and running the caches.
std::string DescribePerf(const VideoCore::Perf::Snapshot& d, double interval_ms) {
    const double idle = Ms(d, Counter::GpuThreadIdleUs);
    const double fence = Ms(d, Counter::FenceWaitUs);
    const double pipelines = Ms(d, Counter::PipelineStallUs);
    const double flushes = Ms(d, Counter::GpuThreadFlushUs);
    const double creates = Ms(d, Counter::ResourceCreateUs);
    const double decodes = Ms(d, Counter::TextureDecodeUs);
    const double work =
        std::max(0.0, interval_ms - idle - fence - pipelines - flushes - creates - decodes);
    const auto get = [&d](Counter counter) { return VideoCore::Perf::Get(d, counter); };
    return fmt::format(
        "GPU thread: idle {:.1f} ms, fence waits {} ({:.1f} ms), pipeline stalls {} ({:.1f} ms), "
        "guest flushes {} ({:.1f} ms), new resources {} ({:.1f} ms), CPU texture decode {:.1f} ms, "
        "other work {:.1f} ms | GPU busy {:.1f} ms, {} submits | {} draws, {} dispatches | "
        "uploads {} ({:.2f} MiB, {} GPU-decoded), downloads {} ({:.2f} MiB) | staging: {} "
        "dedicated ({:.2f} MiB), {} ring waits",
        idle, get(Counter::FenceWaits), fence, get(Counter::PipelineStalls), pipelines,
        get(Counter::GpuThreadFlushes), flushes, get(Counter::ResourcesCreated), creates, decodes,
        work,
        Ms(d, Counter::GpuBusyUs), get(Counter::Submits), get(Counter::Draws),
        get(Counter::Dispatches), get(Counter::TextureUploads),
        static_cast<double>(get(Counter::TextureUploadBytes)) / (1024.0 * 1024.0),
        get(Counter::TextureGpuDecodes), get(Counter::TextureDownloads),
        static_cast<double>(get(Counter::TextureDownloadBytes)) / (1024.0 * 1024.0),
        get(Counter::StagingDedicated),
        static_cast<double>(get(Counter::StagingDedicatedBytes)) / (1024.0 * 1024.0),
        get(Counter::StagingStreamWaits));
}

/// Where the GPU thread's draws spend their time (per draw, in microseconds), what is left outside
/// draws, clears and dispatches (Maxwell methods, macros, DMA, presenting), and how long the guest
/// waited for the GPU to signal its fences.
std::string DescribeDrawCosts(const VideoCore::Perf::Snapshot& d, double interval_ms) {
    if (!VideoCore::Perf::DetailedGpuProfileEnabled()) {
        return "detailed per-draw profiling off (boot.cfg gpu_profile=1 enables it)";
    }
    const auto get = [&d](Counter counter) { return VideoCore::Perf::Get(d, counter); };
    const u64 draws = std::max<u64>(1, get(Counter::Draws));
    const auto per_draw = [&](Counter counter) {
        return static_cast<double>(get(counter)) / 1000.0 / static_cast<double>(draws);
    };
    const auto ns_ms = [&](Counter counter) { return static_cast<double>(get(counter)) / 1e6; };
    const double draw_ms = ns_ms(Counter::DrawNs);
    const double clear_ms = ns_ms(Counter::ClearNs);
    const double dispatch_ms = ns_ms(Counter::DispatchNs);
    const double submit_ms = ns_ms(Counter::SubmitListNs);
    const double view_total_ms = ns_ms(Counter::TextureCacheViewCreateNs);
    const double view_setup_ms = ns_ms(Counter::TextureCacheViewSetupNs);
    const double view_reinterpret_ms = ns_ms(Counter::TextureCacheViewReinterpretNs);
    const double view_srv_ms = ns_ms(Counter::TextureCacheViewSrvNs);
    const double view_attachment_ms = ns_ms(Counter::TextureCacheViewAttachmentNs);
    const double view_slot_insert_ms = ns_ms(Counter::TextureCacheViewSlotInsertNs);
    const double view_slot_grow_ms = ns_ms(Counter::TextureCacheViewSlotGrowNs);
    const double view_slot_clock_ms = ns_ms(Counter::TextureCacheViewSlotClockNs);
    const double view_slot_free_ms = ns_ms(Counter::TextureCacheViewSlotFreeNs);
    const double view_slot_construct_ms = ns_ms(Counter::TextureCacheViewSlotConstructNs);
    const double view_slot_bit_ms = ns_ms(Counter::TextureCacheViewSlotBitNs);
    const double view_index_ms = ns_ms(Counter::TextureCacheViewIndexNs);
    const double view_base_ms = ns_ms(Counter::TextureCacheViewBaseNs);
    const double view_compatibility_ms = ns_ms(Counter::TextureCacheViewCompatibilityNs);
    const double view_placement_ms = std::max(
        0.0, view_slot_insert_ms + view_slot_grow_ms - view_base_ms - view_setup_ms -
                 view_reinterpret_ms - view_srv_ms - view_attachment_ms);
    const double submit_overhead = std::max(0.0, submit_ms - draw_ms - clear_ms - dispatch_ms);
    const double outside = std::max(0.0, interval_ms - Ms(d, Counter::GpuThreadIdleUs) -
                                             Ms(d, Counter::FenceWaitUs) -
                                             Ms(d, Counter::GpuThreadFlushUs) - draw_ms -
                                             clear_ms - dispatch_ms);
    return fmt::format(
        "draws {:.1f} ms ({:.1f} us each: textures {:.1f}, buffers {:.1f}, descriptors {:.1f}, "
        "targets {:.1f} [update {:.1f}, feedback {:.1f}, framebuffer {:.1f}, transitions {:.1f}], "
        "samplers {:.1f}, record {:.1f}), clears {:.1f} ms, dispatches {:.1f} ms, "
        "outside them {:.1f} ms | pipeline fast path {} hits / {} misses | CBV {} ({:.1f} ms: "
        "{} streamed, {} persistent, {} null), view copies {} ({:.1f} ms) | guest waited for "
        "the GPU {} times ({:.1f} ms in all) | commands: {} submit lists {:.1f} ms ({:.1f} ms "
        "outside draw/clear/dispatch), {} ticks {:.1f} ms, {} invalidations {:.1f} ms | uploads: "
        "{} maps {:.1f} ms, {} repacks {:.1f} ms, {} copies {:.1f} ms | texture cache: "
        "{} finds {:.1f} ms, {} inserts {:.1f} ms (overlap {:.1f}, image {:.1f}, refresh "
        "{:.1f}, register {:.1f}), {} views {:.1f} ms [setup {:.1f}, reinterpret {:.1f}, "
        "base {:.1f} (compat {:.1f}), SRV {:.1f}, attachments {:.1f}, slot {:.1f}, "
        "grow {} / {:.1f}, placement {:.1f} [clock {:.1f}, free {:.1f}, construct {:.1f}, bit "
        "{:.1f}], index {:.1f}], "
        "staging {:.1f}, unswizzle {:.1f}, "
        "backend {:.1f} ms | DMA: "
        "puller {} / {:.1f} ms, macros {} / {:.1f} ms, Maxwell {} / {:.1f} ms, compute {} / "
        "{:.1f} ms, copies {} / {:.1f} ms, other {} / {:.1f} ms | Maxwell dirty: {} changed, "
        "{} identical skipped",
        draw_ms, per_draw(Counter::DrawNs), per_draw(Counter::DrawTexturesNs),
        per_draw(Counter::DrawBuffersNs), per_draw(Counter::DrawDescriptorsNs),
        per_draw(Counter::DrawTargetsNs), per_draw(Counter::DrawUpdateTargetsNs),
        per_draw(Counter::DrawFeedbackNs), per_draw(Counter::DrawFramebufferNs),
        per_draw(Counter::DrawImageTransitionsNs), per_draw(Counter::DrawSamplersNs),
        per_draw(Counter::DrawRecordNs), clear_ms, dispatch_ms, outside,
        get(Counter::PipelineFastHits), get(Counter::PipelineFastMisses),
        get(Counter::CbvCreates), ns_ms(Counter::CbvCreateNs), get(Counter::CbvStreamed),
        get(Counter::CbvPersistent), get(Counter::CbvNull), get(Counter::ViewCopies),
        ns_ms(Counter::ViewCopyNs),
        get(Counter::GuestGpuWaits), Ms(d, Counter::GuestGpuWaitUs), get(Counter::SubmitLists),
        submit_ms, submit_overhead, get(Counter::GpuTicks), ns_ms(Counter::GpuTickNs),
        get(Counter::CacheInvalidations), ns_ms(Counter::CacheInvalidationNs),
        get(Counter::TextureUploadMaps), ns_ms(Counter::TextureUploadMapNs),
        get(Counter::TextureUploadRepacks), ns_ms(Counter::TextureUploadRepackNs),
        get(Counter::TextureUploadCopies), ns_ms(Counter::TextureUploadRecordNs),
        get(Counter::TextureCacheFinds), ns_ms(Counter::TextureCacheFindNs),
        get(Counter::TextureCacheInserts), ns_ms(Counter::TextureCacheInsertNs),
        ns_ms(Counter::TextureCacheOverlapNs), ns_ms(Counter::TextureCacheImageCreateNs),
        ns_ms(Counter::TextureCacheRefreshNs), ns_ms(Counter::TextureCacheRegisterNs),
        get(Counter::TextureCacheViewsCreated), view_total_ms, view_setup_ms, view_reinterpret_ms,
        view_base_ms, view_compatibility_ms, view_srv_ms, view_attachment_ms, view_slot_insert_ms,
        get(Counter::TextureCacheViewSlotGrows), view_slot_grow_ms, view_placement_ms,
        view_slot_clock_ms, view_slot_free_ms, view_slot_construct_ms, view_slot_bit_ms,
        view_index_ms,
        ns_ms(Counter::TextureCacheStagingNs), ns_ms(Counter::TextureCacheUnswizzleNs),
        ns_ms(Counter::TextureCacheBackendUploadNs),
        get(Counter::DmaPullerCalls), ns_ms(Counter::DmaPullerNs), get(Counter::DmaMacroCalls),
        ns_ms(Counter::DmaMacroNs), get(Counter::DmaMaxwellCalls), ns_ms(Counter::DmaMaxwellNs),
        get(Counter::DmaComputeCalls), ns_ms(Counter::DmaComputeNs), get(Counter::DmaCopyCalls),
        ns_ms(Counter::DmaCopyNs), get(Counter::DmaOtherCalls), ns_ms(Counter::DmaOtherNs),
        get(Counter::MaxwellDirtyChanged), get(Counter::MaxwellDirtyUnchanged));
}

std::string DescribeGuestWaitSites() {
    const auto sites = VideoCore::Perf::TakeGuestWaitSites();
    std::array<size_t, VideoCore::Perf::NUM_GUEST_SYNCPOINTS> order{};
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&sites](size_t left, size_t right) {
        return sites[left].total_us > sites[right].total_us;
    });
    std::string result;
    size_t emitted{};
    for (const size_t id : order) {
        if (sites[id].count == 0 || emitted == 6) {
            break;
        }
        if (!result.empty()) {
            result += "; ";
        }
        result += fmt::format("id {}: {} waits, {:.1f} ms total, {:.1f} ms max", id,
                              sites[id].count, static_cast<double>(sites[id].total_us) / 1000.0,
                              static_cast<double>(sites[id].max_us) / 1000.0);
        ++emitted;
    }
    return result.empty() ? "none" : result;
}

std::string DescribeMacroProfiles() {
    auto profiles = VideoCore::Perf::TakeMacroProfiles();
    std::ranges::sort(profiles, [](const auto& left, const auto& right) {
        return left.total_ns - std::min(left.total_ns, left.nested_ns) >
               right.total_ns - std::min(right.total_ns, right.nested_ns);
    });
    std::string result;
    for (size_t index = 0; index < std::min<size_t>(profiles.size(), 6); ++index) {
        const auto& profile = profiles[index];
        const double exclusive_ms =
            static_cast<double>(profile.total_ns - std::min(profile.total_ns, profile.nested_ns)) /
            1e6;
        if (!result.empty()) {
            result += "; ";
        }
        result += fmt::format("m=0x{:x} h={:016x}: {} calls, {:.1f} ms exclusive, {:.1f} ms "
                              "nested, {:.2f} ms max",
                              profile.method, profile.hash, profile.count, exclusive_ms,
                              static_cast<double>(profile.nested_ns) / 1e6,
                              static_cast<double>(profile.max_ns) / 1e6);
    }
    return result.empty() ? "none" : result;
}

/// The frame chain over a window: frames the game queued, vsyncs (and those lost to a late one),
/// the VSyncThread waiting for the GPU thread, how long composites took to run after being
/// requested, and the game waiting for a free framebuffer.
std::string DescribeFrameChain(const VideoCore::Perf::Snapshot& d) {
    const auto get = [&d](Counter counter) { return VideoCore::Perf::Get(d, counter); };
    const auto average_ms = [&](Counter us, Counter count) {
        return get(count) != 0 ? Ms(d, us) / static_cast<double>(get(count)) : 0.0;
    };
    return fmt::format(
        "game queued {} frames | vsyncs {} ({} lost), {} with a new frame; waited for the GPU "
        "thread {:.1f} ms | composites {} ({:.1f} ms after the request on average, {} over a "
        "frame) | game waited for a free framebuffer {} times ({:.1f} ms) | forced {} swap "
        "intervals to one | emulated cores idle {:.1f} ms in all",
        get(Counter::GuestFramesQueued), get(Counter::Vsyncs), get(Counter::VsyncsLost),
        get(Counter::VsyncFrames), Ms(d, Counter::VsyncComposeWaitUs), get(Counter::Composites),
        average_ms(Counter::CompositeLatencyUs, Counter::Composites), get(Counter::CompositesLate),
        get(Counter::GuestDequeueWaits), Ms(d, Counter::GuestDequeueWaitUs),
        get(Counter::GuestSwapIntervalOverrides),
        Ms(d, Counter::GuestCoreIdleUs));
}

/// The largest share of a slow frame, in words.
const char* LikelyCause(const VideoCore::Perf::Snapshot& d, double interval_ms) {
    const std::array<std::pair<double, const char*>, 7> shares{{
        {Ms(d, Counter::GpuThreadIdleUs), "guest CPU (the GPU thread waited for work)"},
        {Ms(d, Counter::FenceWaitUs), "GPU (the GPU thread waited for the GPU)"},
        {Ms(d, Counter::PipelineStallUs), "pipeline builds"},
        {Ms(d, Counter::GpuThreadFlushUs), "guest reading back GPU memory"},
        {Ms(d, Counter::ResourceCreateUs), "creating resources"},
        {Ms(d, Counter::TextureDecodeUs), "decoding textures on the CPU"},
        {0.0, "recording draws / texture and buffer caches"},
    }};
    double accounted = 0.0;
    for (const auto& share : shares) {
        accounted += share.first;
    }
    const double other = std::max(0.0, interval_ms - accounted);
    const auto* best = &shares.back();
    double best_ms = other;
    for (const auto& share : shares) {
        if (share.first > best_ms) {
            best_ms = share.first;
            best = &share;
        }
    }
    return best->second;
}

} // Anonymous namespace

void RendererD3D12::ReportPerf(double interval_ms) {
    constexpr double REPORT_MS = 100.0;
    const VideoCore::Perf::Snapshot now = VideoCore::Perf::Read();
    const VideoCore::Perf::Snapshot delta = Delta(now, perf_frame);
    perf_frame = now;
    const auto time = std::chrono::steady_clock::now();
    // At most one per second, so a stretch of slow frames does not flood the log.
    if (interval_ms < REPORT_MS || time - last_hitch_report < std::chrono::seconds{1}) {
        return;
    }
    last_hitch_report = time;
    LOG_INFO(Render, "D3D12 hitch: {:.0f} ms frame, likely {} | {}", interval_ms,
             LikelyCause(delta, interval_ms), DescribePerf(delta, interval_ms));
    LOG_INFO(Render, "D3D12 hitch detail: {}", DescribeDrawCosts(delta, interval_ms));
}

void RendererD3D12::ReportPerfWindow(u32 frames, double total_ms) {
    const VideoCore::Perf::Snapshot now = VideoCore::Perf::Read();
    const VideoCore::Perf::Snapshot delta = Delta(now, perf_window);
    perf_window = now;
    LOG_INFO(Render, "D3D12 perf over {} frames ({:.0f} ms), mostly {} | {}", frames, total_ms,
             LikelyCause(delta, total_ms), DescribePerf(delta, total_ms));
    LOG_INFO(Render, "D3D12 GPU thread: {}", DescribeDrawCosts(delta, total_ms));
    LOG_INFO(Render, "D3D12 frame chain: {}", DescribeFrameChain(delta));
    LOG_INFO(Render, "D3D12 depth feedback: {} GPU copies in total",
             texture_cache_runtime.DepthFeedbackCopies());
    if (Core::CpuProfile::Enabled()) {
        const auto jit = Dynarmic::JitProfile::Take();
        const auto measurement = [&](Dynarmic::JitProfile::Phase phase) -> const auto& {
            return jit[static_cast<size_t>(phase)];
        };
        using Phase = Dynarmic::JitProfile::Phase;
        LOG_INFO(Render, "D3D12 guest JIT compilation: {} blocks, {:.1f} ms total; "
                         "translate {:.1f} ms, optimize {:.1f} ms, emit {:.1f} ms; "
                         "{} page protection calls, {:.1f} ms; {} invalidations, {:.1f} ms "
                         "(elapsed across cores; phases/protection overlap)",
                 measurement(Phase::Compile).calls, measurement(Phase::Compile).ns / 1.0e6,
                 measurement(Phase::Translate).ns / 1.0e6,
                 measurement(Phase::Optimize).ns / 1.0e6,
                 measurement(Phase::Emit).ns / 1.0e6,
                 measurement(Phase::Protect).calls, measurement(Phase::Protect).ns / 1.0e6,
                 measurement(Phase::Invalidate).calls, measurement(Phase::Invalidate).ns / 1.0e6);
        for (size_t i = static_cast<size_t>(Phase::WriteOpen); i < jit.size(); ++i) {
            LOG_INFO(Render, "D3D12 JIT emission {}: {} calls, {:.3f} ms (nested elapsed)",
                     Dynarmic::JitProfile::names[i], jit[i].calls, jit[i].ns / 1.0e6);
        }
        using Core::CpuProfile::Counter;
        for (size_t core = 0; core < Core::CpuProfile::cores.size(); ++core) {
            const auto cpu = Core::CpuProfile::Take(core);
            LOG_INFO(Render, "D3D12 guest CPU core {}: {} JIT runs, {:.1f} ms elapsed in Run "
                             "(includes translation/callbacks/preemption), {} code words, "
                             "{} slow reads ({} vectors), {} slow writes, {} clock reads",
                     core, Get(cpu, Counter::Runs), Get(cpu, Counter::RunNs) / 1.0e6,
                     Get(cpu, Counter::CodeWords), Get(cpu, Counter::Reads),
                     Get(cpu, Counter::Reads128),
                     Get(cpu, Counter::Writes), Get(cpu, Counter::ClockReads));
            const u64 samples = Get(cpu, Counter::ReadSamples);
            LOG_INFO(Render, "D3D12 guest CPU core {} callbacks: {} read samples, {:.0f} ns "
                             "mean sampled read; {} flush-area misses, {:.1f} ms "
                             "(cache locks and possible GPU downloads included)",
                     core, samples, samples ? static_cast<double>(Get(cpu, Counter::ReadSampleNs)) /
                                                 samples : 0.0,
                     Get(cpu, Counter::FlushChecks), Get(cpu, Counter::FlushCheckNs) / 1.0e6);
        }
    }
    LOG_INFO(Render, "D3D12 guest GPU wait sites: {}", DescribeGuestWaitSites());
    LOG_INFO(Render, "D3D12 macro profiles: {}", DescribeMacroProfiles());
    // Who submits and waits (S/W, count, eden-uwp.exe RVAs from the innermost caller out).
    LOG_INFO(Render, "D3D12 sync sites: {}", scheduler.TakeSyncSites(6));
    const DXGI_QUERY_VIDEO_MEMORY_INFO video = device.QueryVideoMemory();
    LOG_INFO(Render,
             "D3D12 memory: GPU {} MiB, DXGI budget {} MiB, cache budget {} MiB, caches see {} MiB used",
             video.CurrentUsage >> 20, video.Budget >> 20, device.CacheMemoryBudget() >> 20,
             device.CacheMemoryUsage() >> 20);
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
    scheduler.SetPipelineState(blit_pipeline.Get());
    // tex_scale, tex_offset
    cmd->SetGraphicsRoot32BitConstants(0, 4, tex_scale_offset.data(), 0);
    const u32 runtime_data[12]{};
    cmd->SetGraphicsRoot32BitConstants(1, 12, runtime_data, 0);
    cmd->SetGraphicsRootDescriptorTable(2, srv_table);
    cmd->SetGraphicsRootDescriptorTable(3, sampler_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    DrawShaderIndicator(cmd, rtv);
    DrawPerformanceOverlay(cmd, rtv);

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
    barrier = Transition(image, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &barrier);
    DrawPerformanceOverlay(cmd, back_buffer_rtvs[swapchain.CurrentIndex()]);
    barrier = Transition(image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cmd->ResourceBarrier(1, &barrier);
}

void RendererD3D12::DrawPerformanceOverlay(ID3D12GraphicsCommandList* cmd,
                                          D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    auto& hud = performance_overlay;
    const auto now = std::chrono::steady_clock::now();
    const bool first = hud.sample_time == std::chrono::steady_clock::time_point{};
    const double elapsed_ms = std::chrono::duration<double, std::milli>(now - hud.sample_time).count();
    if (first || elapsed_ms >= 500.0) {
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool cpu_valid = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const auto ticks = [](FILETIME time) {
            return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        const u64 cpu_ticks = ticks(kernel) + ticks(user);
        const u64 gpu_us = VideoCore::Perf::counters[
            static_cast<size_t>(VideoCore::Perf::Counter::GpuBusyUs)].load(std::memory_order_relaxed);
        std::array<std::string, 4> lines{"FPS --", "FRAME -- MS MAX --", "CPU --", "GPUQ --"};
        if (!first && hud.frames != 0) {
            const double frames = static_cast<double>(hud.frames);
            lines[0] = fmt::format("FPS {:.1f}", frames * 1000.0 / elapsed_ms);
            lines[1] = fmt::format("FRAME {:.1f} MS MAX {:.1f}", hud.last_frame_ms, hud.max_frame_ms);
            // Process CPU includes all threads; show equivalent cores rather than an ambiguous %.
            if (cpu_valid && hud.cpu_valid && cpu_ticks >= hud.cpu_ticks) {
                const double cpu_ms = static_cast<double>(cpu_ticks - hud.cpu_ticks) / 10000.0;
                lines[2] = fmt::format("CPU {:.2f} CORES {:.1f} MS/F", cpu_ms / elapsed_ms,
                                       cpu_ms / frames);
            }
            if (scheduler.HasGpuTimestamps()) {
                const double gpu_ms = static_cast<double>(gpu_us - hud.gpu_us) / 1000.0;
                // Completed direct-queue time, delayed by frames in flight; not whole-GPU utilization.
                lines[3] = fmt::format("GPUQ {:.0f}% {:.1f} MS/F", gpu_ms * 100.0 / elapsed_ms,
                                       gpu_ms / frames);
            }
        }
        const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
        const LONG pad = 3 * cell;
        const LONG right = static_cast<LONG>(swapchain.Width()) - 8 * cell;
        const LONG top = 8 * cell;
        size_t chars = 0;
        for (const auto& line : lines) {
            chars = std::max(chars, line.size());
        }
        const LONG left = right - TextWidth(chars, cell) - 2 * pad;
        hud.panel = {left, top, right, top + 2 * pad + 4 * 7 * cell - 2 * cell};
        hud.text.clear();
        hud.text.reserve(1536);
        for (size_t row = 0; row < lines.size(); ++row) {
            AppendText(hud.text, lines[row], left + pad,
                       top + pad + static_cast<LONG>(row) * 7 * cell, cell);
        }
        hud.sample_time = now;
        hud.cpu_ticks = cpu_ticks;
        hud.cpu_valid = cpu_valid;
        hud.gpu_us = gpu_us;
        hud.frames = 0;
        hud.max_frame_ms = 0;
    }
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&hud.panel, 1});
    ClearRects(cmd, rtv, OVERLAY_TEXT, hud.text);
    DrawTraceIndicator(cmd, rtv);
}

void RendererD3D12::DrawTraceIndicator(ID3D12GraphicsCommandList* cmd,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    using VideoCore::FrameTrace::CaptureState;
    const auto status = VideoCore::FrameTrace::GetCaptureStatus();
    if (status.state == CaptureState::Idle) {
        return;
    }
    auto& hud = performance_overlay;
    const u32 tenths = (status.vsyncs_remaining + 5) / 6;
    const u64 key = (static_cast<u64>(status.id) << 32) |
                    (static_cast<u64>(status.state) << 24) | tenths;
    const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
    const LONG top = hud.panel.bottom + 2 * cell;
    if (key != hud.trace_key || top != hud.trace_top) {
        std::string label;
        switch (status.state) {
        case CaptureState::Recording:
            label = fmt::format("T {} CAPTURANDO {:.1f} S", status.id, tenths / 10.0);
            break;
        case CaptureState::Saving: label = fmt::format("T {} GUARDANDO", status.id); break;
        case CaptureState::Saved: label = fmt::format("T {} GUARDADA", status.id); break;
        case CaptureState::Truncated: label = fmt::format("T {} TRUNCADA", status.id); break;
        default: return;
        }
        const LONG pad = 3 * cell;
        const LONG right = hud.panel.right;
        const LONG left = right - TextWidth(label.size(), cell) - 2 * pad;
        hud.trace_panel = {left, top, right, top + 2 * pad + 5 * cell};
        hud.trace_text.clear();
        hud.trace_text.reserve(256);
        AppendText(hud.trace_text, label, left + pad, top + pad, cell);
        hud.trace_key = key;
        hud.trace_top = top;
    }
    constexpr std::array<float, 4> saved_color{0.3f, 1.0f, 0.45f, 1.0f};
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&hud.trace_panel, 1});
    ClearRects(cmd, rtv, status.state == CaptureState::Saved ? saved_color : OVERLAY_ACCENT,
               hud.trace_text);
}

void RendererD3D12::DrawShaderIndicator(ID3D12GraphicsCommandList* cmd,
                                        D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    const int building = gpu.ShaderNotify().ShadersBuilding();
    if (building <= 0) {
        indicator_frames = 0;
        return;
    }
    ++indicator_frames;
    // 3-pixel cells at 1080p: the panel is 27 pixels tall, out of the way in the corner.
    const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
    const std::string count = std::to_string(building);
    const LONG pad = 3 * cell;
    const LONG dots_width = 5 * cell;
    const LONG width = pad + dots_width + 3 * cell + TextWidth(count.size(), cell) + pad;
    const LONG height = pad + 5 * cell + pad;
    const LONG right = static_cast<LONG>(swapchain.Width()) - 8 * cell;
    const LONG bottom = static_cast<LONG>(swapchain.Height()) - 8 * cell;
    const LONG left = right - width;
    const LONG top = bottom - height;
    const D3D12_RECT panel{left, top, right, bottom};
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&panel, 1});

    // Three dots on the middle row; the lit one moves every 8 presented frames.
    std::vector<D3D12_RECT> dim;
    std::vector<D3D12_RECT> lit;
    const LONG dot_y = top + pad + 2 * cell;
    for (LONG dot = 0; dot < 3; ++dot) {
        const LONG x = left + pad + dot * 2 * cell;
        (dot == static_cast<LONG>((indicator_frames / 8) % 3) ? lit : dim)
            .push_back({x, dot_y, x + cell, dot_y + cell});
    }
    ClearRects(cmd, rtv, OVERLAY_DIM, dim);
    ClearRects(cmd, rtv, OVERLAY_ACCENT, lit);

    std::vector<D3D12_RECT> text;
    AppendText(text, count, left + pad + dots_width + 3 * cell, top + pad, cell);
    ClearRects(cmd, rtv, OVERLAY_TEXT, text);
}

void ShowCpuLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowLoadProgress(done, total, "CPU JIT");
    }
}

void RendererD3D12::ShowLoadProgress(size_t done, size_t total, std::string_view phase) {
    if (present_failed || total == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (done != total && now - last_load_present < std::chrono::milliseconds{33}) {
        return;
    }
    last_load_present = now;
    try {
        const u32 index = swapchain.CurrentIndex();
        scheduler.Wait(present_ticks[index]);
        ID3D12Resource* const image = swapchain.Image(index);
        ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
        D3D12_RESOURCE_BARRIER barrier =
            Transition(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &barrier);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = back_buffer_rtvs[index];
        constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->ClearRenderTargetView(rtv, black, 0, nullptr);

        // A bar across the middle 40% of the screen and "done/total" centered under it.
        const LONG screen_width = static_cast<LONG>(swapchain.Width());
        const LONG screen_height = static_cast<LONG>(swapchain.Height());
        const LONG cell = std::max<LONG>(2, screen_height / 270);
        const LONG bar_width = screen_width * 2 / 5;
        const LONG bar_left = (screen_width - bar_width) / 2;
        const LONG bar_top = screen_height / 2 - cell;
        const LONG filled = static_cast<LONG>(static_cast<double>(bar_width) *
                                              static_cast<double>(std::min(done, total)) /
                                              static_cast<double>(total));
        const D3D12_RECT bar{bar_left, bar_top, bar_left + bar_width, bar_top + 2 * cell};
        const D3D12_RECT fill{bar_left, bar_top, bar_left + filled, bar_top + 2 * cell};
        ClearRects(cmd, rtv, OVERLAY_DIM, {&bar, 1});
        if (filled > 0) {
            ClearRects(cmd, rtv, OVERLAY_ACCENT, {&fill, 1});
        }
        const std::string label = fmt::format("{} {}/{}", phase, done, total);
        std::vector<D3D12_RECT> text;
        AppendText(text, label, (screen_width - TextWidth(label.size(), cell)) / 2,
                   bar_top + 6 * cell, cell);
        ClearRects(cmd, rtv, OVERLAY_TEXT, text);

        barrier = Transition(image, D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PRESENT);
        cmd->ResourceBarrier(1, &barrier);
        Present(index);
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: shader cache progress not shown: {}", e.what());
    }
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

void RendererD3D12::WriteFrameDump(StagingBufferRef& readback, u32 dump_index) {
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
        Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
        (dump_index == 0 ? std::string("frame.bmp") : fmt::format("frame_{}.bmp", dump_index));
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
