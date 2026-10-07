// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <exception>
#include <utility>

#include "common/logging.h"
#include "core/frontend/emu_window.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host_shaders/blit_color_float_frag_spv.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"

// Presentation of the GPU-rendered guest image: a full-screen blit into the back buffer.

namespace D3D12 {

bool RendererD3D12::CreateBlitPipeline() {
    if (!shader_compiler.IsAvailable()) {
        LOG_WARNING(Render, "D3D12: no shader path, presenting through the CPU");
        return false;
    }
    try {
        ID3D12Device* const dev = device.Get();

        // Eden's blit shaders, exactly as the Vulkan backend uses them, linked as guest pipelines
        // will be. No Y flip: the blit has always run unflipped (see docs/xbox/xbox_d3d12_phase4.md).
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
        blit_root_signature = CreateRootSignature(dev, root_desc, "present blit root signature");

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

bool RendererD3D12::CompositeAccelerated(const Tegra::FramebufferConfig& framebuffer,
                                         PresentFrame& frame) {
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
    RecordBlit(frame, srv_table, sampler_table, scale_offset);
    if (!logged_accelerated) {
        LOG_INFO(Render, "D3D12: presenting the GPU-rendered guest image ({}x{} image, {}x{} "
                 "framebuffer)", texture->width, texture->height, framebuffer.width,
                 framebuffer.height);
        logged_accelerated = true;
    }
    return true;
}

void RendererD3D12::RecordBlit(PresentFrame& frame, D3D12_GPU_DESCRIPTOR_HANDLE srv_table,
                               D3D12_GPU_DESCRIPTOR_HANDLE sampler_table,
                               const std::array<float, 4>& tex_scale_offset) {
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    frame.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = frame.rtv;
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
    DrawGameMenu(cmd, rtv);
}

} // namespace D3D12
