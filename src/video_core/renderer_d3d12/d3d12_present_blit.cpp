// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <exception>
#include <optional>
#include <span>
#include <utility>

#include "common/logging.h"
#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host_shaders/blit_color_float_frag_spv.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"

// Presentation of the guest image: a full-screen bilinear blit into the back buffer, or FSR 1
// when it is selected (d3d12_present_fsr.h).

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
    // Same crop and flips as the Vulkan presenter (Tegra::NormalizeCrop).
    RecordComposite(frame, {.srv = texture->srv,
                            .width = texture->width,
                            .height = texture->height,
                            .crop = Tegra::NormalizeCrop(framebuffer, texture->width,
                                                         texture->height)});
    if (!logged_accelerated) {
        LOG_INFO(Render, "D3D12: presenting the GPU-rendered guest image ({}x{} image, {}x{} "
                 "framebuffer)", texture->width, texture->height, framebuffer.width,
                 framebuffer.height);
        logged_accelerated = true;
    }
    return true;
}

void RendererD3D12::RecordComposite(PresentFrame& frame, const CompositeSource& source) {
    // Everything that may flush the list comes first: the FSR target (its creation defers the old
    // one to the current tick), then every descriptor of the frame. The source and, with FSR, the
    // EASU target go into one range, so no second allocation can retire the first.
    std::optional<Fsr::Plan> fsr = PlanFsr(source);
    if (fsr && !present_fsr.PrepareTarget(*fsr)) {
        fsr.reset();
    }
    constexpr u64 LINEAR_SAMPLER_KEY = 0;
    const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table =
        sampler_heap.GetTable({&LINEAR_SAMPLER_KEY, 1}, {&linear_sampler, 1});
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> srvs{source.srv, present_fsr.TargetSrv()};
    const u32 srv_count = fsr ? 2 : 1;
    const D3D12_GPU_DESCRIPTOR_HANDLE source_table =
        descriptor_ring.Upload(std::span{srvs}.first(srv_count));

    // Descriptor heaps are command-list state; nothing below can reset the list.
    CommandStream* const cmd = scheduler.CommandList();
    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);
    if (fsr) {
        const D3D12_GPU_DESCRIPTOR_HANDLE target_table{source_table.ptr +
                                                       descriptor_ring.Stride()};
        present_fsr.RecordEasu(cmd, *fsr, source_table, sampler_table);
        BeginComposite(cmd, frame);
        present_fsr.RecordRcas(cmd, *fsr, target_table);
    } else {
        BeginComposite(cmd, frame);
        RecordBlit(cmd, source, source_table, sampler_table);
    }
    // After the scaling, as ffx_fsr1.h asks of the UI.
    DrawShaderIndicator(cmd, frame.rtv);
    DrawPerformanceOverlay(cmd, frame.rtv);
    DrawGameMenu(cmd, frame);
}

std::optional<Fsr::Plan> RendererD3D12::PlanFsr(const CompositeSource& source) {
    // Read on the GPU thread, which is also where the frontend changes it (RunOnGpuThread).
    // A failure to build FSR is logged once by PresentFsr, and FSR then counts as off.
    const bool enabled =
        Settings::values.scaling_filter.GetValue() == Settings::ScalingFilter::Fsr &&
        present_fsr.EnsurePipelines();
    const auto& screen = render_window.GetFramebufferLayout().screen;
    std::optional<Fsr::Plan> plan;
    if (enabled) {
        plan = Fsr::MakePlan(source.width, source.height, source.crop, screen.GetWidth(),
                             screen.GetHeight(), Settings::values.fsr_sharpening_slider.GetValue());
    }

    // One line whenever what the screen shows changes, not every frame.
    const FsrLogState state{
        .active = plan.has_value(),
        .source_width = source.width,
        .source_height = source.height,
        .output_width = screen.GetWidth(),
        .output_height = screen.GetHeight(),
        .sharpness = plan ? plan->sharpness : 0.0f,
    };
    if (enabled && fsr_logged != state) {
        if (plan) {
            LOG_INFO(Render, "D3D12: FSR 1 {}x{} -> {}x{} ({:.2f}x{:.2f}), RCAS {:.2f} stops{}",
                     source.width, source.height, plan->output_width, plan->output_height,
                     plan->scale_x, plan->scale_y, plan->sharpness,
                     plan->WithinTunedRange() ? "" : "; beyond EASU's 2x, softer");
        } else {
            LOG_INFO(Render, "D3D12: FSR 1 selected, bilinear blit: the {}x{} screen does not "
                     "enlarge the {}x{} image", screen.GetWidth(), screen.GetHeight(),
                     source.width, source.height);
        }
        fsr_logged = state;
    } else if (!enabled && fsr_logged) {
        LOG_INFO(Render, "D3D12: FSR 1 off, bilinear blit");
        fsr_logged.reset();
    }
    if (!enabled) {
        present_fsr.ReleaseTarget();
    }
    return plan;
}

void RendererD3D12::BeginComposite(CommandStream* cmd, PresentFrame& frame) {
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
}

void RendererD3D12::RecordBlit(CommandStream* cmd, const CompositeSource& source,
                               D3D12_GPU_DESCRIPTOR_HANDLE source_table,
                               D3D12_GPU_DESCRIPTOR_HANDLE sampler_table) {
    // The quad runs from the bottom of the screen (y = -1 in D3D12 clip space: tex_offset) to the
    // top (tex_offset + tex_scale), so the top edge samples crop.top.
    const Common::Rectangle<f32>& crop = source.crop;
    const std::array<float, 4> tex_scale_offset{crop.right - crop.left, crop.top - crop.bottom,
                                                crop.left, crop.bottom};
    cmd->SetGraphicsRootSignature(blit_root_signature.Get());
    scheduler.SetPipelineState(blit_pipeline.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 4, tex_scale_offset.data(), 0);
    const u32 runtime_data[12]{};
    cmd->SetGraphicsRoot32BitConstants(1, 12, runtime_data, 0);
    cmd->SetGraphicsRootDescriptorTable(2, source_table);
    cmd->SetGraphicsRootDescriptorTable(3, sampler_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace D3D12
